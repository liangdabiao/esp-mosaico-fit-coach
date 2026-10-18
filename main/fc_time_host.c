// SPDX-License-Identifier: Apache-2.0
// main/fc_time_host.c —— 宿主时间：单调时钟 + 系统日历。
#include "fc_time.h"

#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

uint32_t fc_time_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

uint32_t fc_time_ymd(void)
{
    const time_t now = time(NULL);
    struct tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    return (uint32_t)(tm_now.tm_year + 1900) * 10000u +
           (uint32_t)(tm_now.tm_mon + 1) * 100u + (uint32_t)tm_now.tm_mday;
}
