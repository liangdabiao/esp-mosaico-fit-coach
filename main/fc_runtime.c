// SPDX-License-Identifier: Apache-2.0
// main/fc_runtime.c —— 设备服务装配。
//
// 单任务循环：
//   * fc_net 联网（配网门户/STA/SNTP）；
//   * game_wants_capture 为真且非演示模式 → 启动摄像头，按模式节流取帧、
//     硬编 JPEG、喂给当前 provider；provider 回调直接 game_feed_pose；
//   * 设置（模式/地址）变化 → 重建 provider；
//   * 链路状态（net + provider 健康）→ game_set_link。
#include "fc_runtime.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fc_camera.h"
#include "fc_count.h"
#include "fc_net.h"
#include "fc_provider.h"
#include "fc_store.h"
#include "fc_time.h"

static const char *TAG = "fc_runtime";

/* 采样节流：http 模式 300ms/帧；vlm 模式 1000ms/帧（4 帧组批）。 */
#define FC_CADENCE_HTTP_MS 300
#define FC_CADENCE_VLM_MS 1000
#define FC_LOOP_TICK_MS 50
#define FC_RUNTIME_STACK 8192

typedef struct {
    game_handle_t game;
    TaskHandle_t task;
    volatile bool running;
    /* provider 热切换游标 */
    fc_provider_t *provider;
    uint8_t provider_mode;
    char provider_url[FC_URL_MAX];
    /* provider 健康：连续失败次数（≥3 视为失败，恢复即清零）。 */
    uint8_t fail_count;
    bool provider_healthy;
} fc_runtime_t;

static fc_runtime_t s_rt;

static void on_sample(const fc_sample_t *sample, void *ctx)
{
    game_feed_pose((game_handle_t)ctx, sample);
}

static void on_status(bool healthy, void *ctx)
{
    fc_runtime_t *rt = (fc_runtime_t *)ctx;
    if (healthy) {
        rt->fail_count = 0;
        rt->provider_healthy = true;
    } else if (rt->fail_count < 255) {
        rt->fail_count++;
        if (rt->fail_count >= 3) rt->provider_healthy = false;
    }
}

/* 依据设置重建 provider。返回 true 表示当前有可用 provider。 */
static bool ensure_provider(void)
{
    const fc_settings_t *s = game_settings_view(s_rt.game);
    if (!s) return false;

    const uint8_t mode = s->mode;
    const char *url = (mode == FC_MODE_HTTP) ? s->server_url
                      : (mode == FC_MODE_VLM) ? s->vlm_base_url : "";
    const bool same = s_rt.provider && s_rt.provider_mode == mode &&
                      strcmp(s_rt.provider_url, url) == 0;
    if (same) return true;

    if (s_rt.provider) {
        s_rt.provider->del(s_rt.provider);
        s_rt.provider = NULL;
    }
    s_rt.provider_mode = mode;
    snprintf(s_rt.provider_url, sizeof(s_rt.provider_url), "%s", url);
    s_rt.fail_count = 0;
    s_rt.provider_healthy = true;

    if (mode == FC_MODE_HTTP && s->server_url[0] && fc_net_sta_ready()) {
        const fc_provider_cbs_t cbs = {on_sample, on_status, &s_rt};
        s_rt.provider = fc_provider_http_new(s->server_url, &cbs);
    } else if (mode == FC_MODE_VLM && s->vlm_base_url[0] && s->vlm_model[0] &&
               fc_net_sta_ready()) {
        const fc_provider_cbs_t cbs = {on_sample, on_status, &s_rt};
        s_rt.provider = fc_provider_vlm_new(s->vlm_base_url, s->vlm_model, s->vlm_key, &cbs);
    }
    if (mode != FC_MODE_DEMO && !s_rt.provider) {
        ESP_LOGW(TAG, "provider unavailable (mode=%u url=%s sta=%d)", mode,
                 url[0] ? url : "-", (int)fc_net_sta_ready());
    }
    return s_rt.provider != NULL;
}

static void report_link(void)
{
    const fc_settings_t *s = game_settings_view(s_rt.game);
    if (!s) return;
    uint8_t link;
    if (s->mode == FC_MODE_DEMO) {
        link = FC_LINK_OK;
    } else if (!fc_net_sta_ready()) {
        link = fc_net_state() == FC_NET_CONNECTING ? FC_LINK_CONNECTING : FC_LINK_OFF;
    } else if (!s_rt.provider) {
        link = FC_LINK_OFF;
    } else {
        link = s_rt.provider_healthy ? FC_LINK_OK : FC_LINK_ERROR;
    }
    game_set_link(s_rt.game, link);
}

static void runtime_task(void *arg)
{
    (void)arg;
    uint32_t last_feed_ms = 0;

    /* 网络启动推迟到引擎就绪之后（3s）：WiFi 依赖 NVS/事件循环，引擎链路
     * 启动顺序竞态曾导致 app_main 阶段 abort（core dump 定位于
     * start_portal 的 esp_wifi_init）。 */
    vTaskDelay(pdMS_TO_TICKS(3000));
    {
        const esp_err_t err = fc_net_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "net start failed: %s (demo mode continues)",
                     esp_err_to_name(err));
        }
    }

    while (s_rt.running) {
        const fc_settings_t *s = game_settings_view(s_rt.game);
        if (s) {
            const bool want = game_wants_capture(s_rt.game);
            if (want) {
                if (!fc_camera_is_streaming()) {
                    fc_camera_start(); /* 首次阻塞直至成功 */
                }
                ensure_provider();
                if (s_rt.provider) {
                    const uint32_t cadence = (s->mode == FC_MODE_VLM)
                                                 ? FC_CADENCE_VLM_MS
                                                 : FC_CADENCE_HTTP_MS;
                    const uint32_t now = fc_time_ms();
                    if (now - last_feed_ms >= cadence) {
                        mosaico_camera_frame_t frame = {0};
                        if (fc_camera_get_frame(&frame) == ESP_OK) {
                            const uint8_t *jpeg = NULL;
                            size_t jpeg_len = 0;
                            if (fc_camera_encode_jpeg(&frame, &jpeg, &jpeg_len) == ESP_OK) {
                                last_feed_ms = now;
                                /* 采集时间戳：编码完成时刻（≈曝光时刻）。 */
                                s_rt.provider->feed(s_rt.provider, jpeg, jpeg_len, now);
                            }
                            fc_camera_put_frame(&frame);
                        }
                    } else {
                        /* 未到采样点：丢一帧保持队列新鲜。 */
                        mosaico_camera_frame_t frame = {0};
                        if (fc_camera_get_frame(&frame) == ESP_OK) {
                            fc_camera_put_frame(&frame);
                        }
                    }
                }
            } else if (fc_camera_is_streaming()) {
                fc_camera_stop();
                last_feed_ms = 0;
                if (s_rt.provider) {
                    /* 离开训练页：释放 provider（VLM 攒批缓存）。 */
                    s_rt.provider->del(s_rt.provider);
                    s_rt.provider = NULL;
                }
            }
            report_link();
        }
        vTaskDelay(pdMS_TO_TICKS(FC_LOOP_TICK_MS));
    }

    if (s_rt.provider) {
        s_rt.provider->del(s_rt.provider);
        s_rt.provider = NULL;
    }
    if (fc_camera_is_streaming()) fc_camera_stop();
    s_rt.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t fc_runtime_start(game_handle_t game)
{
    if (s_rt.running || !game) return ESP_ERR_INVALID_STATE;
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.game = game;
    s_rt.running = true;
    s_rt.provider_healthy = true;

    if (xTaskCreate(runtime_task, "fc_rt", FC_RUNTIME_STACK, NULL, 5, &s_rt.task) !=
        pdPASS) {
        s_rt.running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void fc_runtime_stop(void)
{
    if (!s_rt.running) return;
    s_rt.running = false;
    while (s_rt.task) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    fc_net_stop();
}
