// SPDX-License-Identifier: Apache-2.0
// main/fc_time.h —— 时间抽象：毫秒单调钟 + 日历日期（设备 esp_timer+SNTP / 宿主 clock）。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单调递增毫秒（设备=esp_timer；宿主=单调时钟）。 */
uint32_t fc_time_ms(void);

/* 当前日期 yyyymmddd 格式 yyyymmdd；时钟未同步时返回 0。 */
uint32_t fc_time_ymd(void);

#ifdef __cplusplus
}
#endif
