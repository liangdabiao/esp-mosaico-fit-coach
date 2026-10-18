// SPDX-License-Identifier: Apache-2.0
// main/fc_store_host.c —— 宿主（模拟器/单测）持久化实现：进程内静态区。
#include "fc_store.h"

#include <string.h>

/* 与设备版行为一致：进程生命周期内保持，重启即失。 */
static fc_settings_t s_settings;
static fc_history_entry_t s_hist[FC_HISTORY_MAX];
static size_t s_hist_count;
static bool s_settings_loaded;

void fc_store_settings_load(fc_settings_t *settings)
{
    if (!s_settings_loaded) {
        fc_settings_defaults(&s_settings);
        s_settings_loaded = true;
    }
    *settings = s_settings;
}

void fc_store_settings_save(const fc_settings_t *settings)
{
    if (!settings) return;
    s_settings = *settings;
    s_settings_loaded = true;
}

void fc_store_history_append(uint32_t ymd, uint16_t reps, uint16_t minutes)
{
    if (s_hist_count < FC_HISTORY_MAX) {
        for (size_t i = s_hist_count; i > 0; i--) s_hist[i] = s_hist[i - 1];
        s_hist_count++;
    } else {
        for (size_t i = FC_HISTORY_MAX - 1; i > 0; i--) s_hist[i] = s_hist[i - 1];
    }
    s_hist[0].ymd = ymd;
    s_hist[0].reps = reps;
    s_hist[0].minutes = minutes;
}

size_t fc_store_history_get(fc_history_entry_t *out, size_t capacity)
{
    if (!out || capacity == 0) return 0;
    const size_t count = s_hist_count < capacity ? s_hist_count : capacity;
    memcpy(out, s_hist, count * sizeof(s_hist[0]));
    return count;
}

uint32_t fc_store_history_today_reps(uint32_t ymd)
{
    uint32_t total = 0;
    for (size_t i = 0; i < s_hist_count; i++) {
        if (s_hist[i].ymd == ymd) total += s_hist[i].reps;
    }
    return total;
}

void fc_store_clear_network(void)
{
    if (!s_settings_loaded) {
        fc_settings_defaults(&s_settings);
        s_settings_loaded = true;
    }
    s_settings.server_url[0] = '\0';
    s_settings.vlm_base_url[0] = '\0';
    s_settings.vlm_model[0] = '\0';
    s_settings.vlm_key[0] = '\0';
    s_settings.wifi_ssid[0] = '\0';
    s_settings.wifi_pass[0] = '\0';
    s_settings.mode = FC_MODE_DEMO;
}
