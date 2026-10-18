// SPDX-License-Identifier: Apache-2.0
// main/fc_net.h —— WiFi 联网与配网门户（设备端）。
//
// 首次上电（NVS 无凭据）或 STA 连接失败：开 SoftAP「fitcoach-XXXX」，
// 手机连接后访问 http://192.168.4.1 填写 WiFi 与服务器/VLM 配置；
// 保存后设备自动重启进入 STA 模式。STA 联网成功后启动 SNTP 对时
// （历史记录按日期归档用）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 网络管理初始化：读 NVS 凭据，有则 STA 连接，无则/失败则开配网门户。
 * 内部创建事件任务，不阻塞调用者。 */
esp_err_t fc_net_start(void);
void fc_net_stop(void);

/* 当前 STA 是否拿到 IP。 */
bool fc_net_sta_ready(void);

typedef enum {
    FC_NET_DOWN = 0,     /* 未连接/门户模式 */
    FC_NET_CONNECTING,   /* 正在连接 */
    FC_NET_READY,        /* 已拿 IP */
} fc_net_state_t;

fc_net_state_t fc_net_state(void);

#ifdef __cplusplus
}
#endif
