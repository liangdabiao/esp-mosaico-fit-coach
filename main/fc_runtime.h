// SPDX-License-Identifier: Apache-2.0
// main/fc_runtime.h —— 设备侧服务装配（WiFi/摄像头/provider → game）。
#pragma once

#include "esp_err.h"
#include "game.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 runtime 任务（摄像头按需启停、provider 按设置热切换）。
 * 失败仅记录：应用以演示模式继续。 */
esp_err_t fc_runtime_start(game_handle_t game);
void fc_runtime_stop(void);

#ifdef __cplusplus
}
#endif
