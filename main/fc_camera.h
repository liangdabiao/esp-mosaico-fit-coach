// SPDX-License-Identifier: Apache-2.0
// main/fc_camera.h —— 摄像头模块封装（mosaico_module_camera + 硬件 JPEG 编码）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_module_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 发现/打开/启动摄像头（重试直至成功；任务内阻塞调用）。 */
esp_err_t fc_camera_start(void);

/* 停流并释放（保留注册）。电源下电由调用方决定。 */
esp_err_t fc_camera_stop(void);

bool fc_camera_is_streaming(void);

/* 取一帧（阻塞至多 frame_timeout）。用完必须 fc_camera_put_frame。 */
esp_err_t fc_camera_get_frame(mosaico_camera_frame_t *out_frame);
esp_err_t fc_camera_put_frame(const mosaico_camera_frame_t *frame);

/* UYVY 帧 → JPEG（硬件编码，质量 80）。输出到内部缓冲区，下次调用覆盖；
 * 出参 out_jpeg 与 out_len 仅在返回 ESP_OK 后有效。 */
esp_err_t fc_camera_encode_jpeg(const mosaico_camera_frame_t *frame,
                                const uint8_t **out_jpeg, size_t *out_len);

#ifdef __cplusplus
}
#endif
