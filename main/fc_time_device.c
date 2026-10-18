// SPDX-License-Identifier: Apache-2.0
// main/fc_time_device.c —— 设备端时间：esp_timer + SNTP 同步后的系统时钟。
#include "fc_time.h"

#include <time.h>

#include "esp_timer.h"

uint32_t fc_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

uint32_t fc_time_ymd(void)
{
    const time_t now = time(NULL);
    if (now < 1000000000LL) return 0; /* 2001-09 之前视为未同步 */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return (uint32_t)(tm_now.tm_year + 1900) * 10000u +
           (uint32_t)(tm_now.tm_mon + 1) * 100u + (uint32_t)tm_now.tm_mday;
}
