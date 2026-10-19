// SPDX-License-Identifier: Apache-2.0
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "fc_runtime.h"
#include "game.h"
#include "game_config.h"
#include "mosaico_game_app.h"

/* Owned by app_main for the application lifetime. Engine callbacks execute on
 * that same task; touch samples reach it through the engine event queue, pose
 * samples through the game's SPSC mailbox. */
static game_handle_t s_game;

static void on_event(const mosaico_device_event_t *event)
{
    if (event && event->type == MOSAICO_DEVICE_EVENT_POINTER)
        game_set_pointer(s_game, event->x, event->y, event->pressed);
}

static void on_update(void)
{
    game_update(s_game);
}

static void on_render(void)
{
    (void)game_render(s_game);
}

static const mosaico_game_app_config_t s_config = {
    .tag = GAME_NAME,
    .window_title = GAME_NAME,
    .canvas_bind = GSP_GAME_BIND_GAME_CANVAS,
    .touch_points = 1,
    .target_fps = GAME_TICK_HZ,
    .gsp_bundle = gsp_bundle_config,
    .on_event = on_event,
    .on_update = on_update,
    .on_render = on_render,
};

void app_main(void)
{
    const game_config_t config = {GAME_WIDTH, GAME_HEIGHT};
    ESP_ERROR_CHECK(game_create(&config, &s_game) ? ESP_OK : ESP_ERR_NO_MEM);
    /* 设备侧服务（音频/网络/摄像头）由 runtime 任务在引擎就绪后启动：
     * app_main 阶段 BSP 的 I2C/电源还没就绪，提前初始化 codec 会静默
     * 失败（与 WiFi 的 abort 崩溃同一类时序坑的静音版）。 */
    fc_runtime_start(s_game);
    /* The engine starts iris_ota_support_start() and marks the image healthy
     * after presenting its first frame; the OTA writer remains in Recovery. */
    esp_err_t result = mosaico_game_app_run(&s_config);
    fc_runtime_stop();
    game_delete(s_game);
    s_game = NULL;
    ESP_ERROR_CHECK(result);
}
