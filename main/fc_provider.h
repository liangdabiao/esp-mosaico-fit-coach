// SPDX-License-Identifier: Apache-2.0
// main/fc_provider.h —— 姿态 provider 抽象（设备端）。
//
// 主路径 fc_provider_http：JPEG 帧推给自建 MediaPipe 服务器，回传角度/状态。
// 备用路径 fc_provider_vlm：OpenAI 兼容视觉模型直连（4 帧组批一次调用）。
// 两者都把结果折算成 fc_sample_t 交给回调（在 runtime 任务上下文调用），
// 由计数内核（fc_count）统一去抖与计数。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "fc_count.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fc_provider fc_provider_t;

typedef void (*fc_provider_sample_cb_t)(const fc_sample_t *sample, void *ctx);
typedef void (*fc_provider_status_cb_t)(bool healthy, void *ctx);

typedef struct {
    fc_provider_sample_cb_t on_sample;
    fc_provider_status_cb_t on_status;
    void *ctx;
} fc_provider_cbs_t;

struct fc_provider {
    const char *name;
    /* 送入一帧硬件 JPEG。同步完成网络往返后才返回（在 runtime 任务调用）。
     * 返回 ESP_OK 表示本次请求完成（结果已走回调），非 OK 表示本帧被
     * 丢弃或请求失败。 */
    esp_err_t (*feed)(fc_provider_t *provider, const uint8_t *jpeg, size_t len,
                      uint32_t t_ms);
    void (*del)(fc_provider_t *provider);
};

/* 构造 MediaPipe 服务器 provider。server_url 形如 http://192.168.1.10:8000。 */
fc_provider_t *fc_provider_http_new(const char *server_url,
                                    const fc_provider_cbs_t *cbs);

/* 构造 VLM provider（OpenAI 兼容 chat/completions + base64 图像，4 帧组批）。 */
fc_provider_t *fc_provider_vlm_new(const char *base_url, const char *model,
                                   const char *api_key,
                                   const fc_provider_cbs_t *cbs);

#ifdef __cplusplus
}
#endif
