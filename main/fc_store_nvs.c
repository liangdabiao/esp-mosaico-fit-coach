// SPDX-License-Identifier: Apache-2.0
// main/fc_store_nvs.c —— 设备端持久化：NVS blob。宿主版见 fc_store_host.c。
#include "fc_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "fc_store";

#define FC_NVS_NS "fit_coach"
#define FC_NVS_KEY_CFG "cfg"
#define FC_NVS_KEY_HIST "hist"

/* fc_settings_defaults 在便携文件 fc_settings.c。 */

static nvs_handle_t open_ns(nvs_open_mode_t mode)
{
    nvs_handle_t h;
    const esp_err_t err = nvs_open(FC_NVS_NS, mode, &h);
    return err == ESP_OK ? h : 0;
}

void fc_store_settings_load(fc_settings_t *settings)
{
    if (!settings) return;
    fc_settings_defaults(settings);
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (!h) return;
    fc_settings_t stored;
    size_t len = sizeof(stored);
    if (nvs_get_blob(h, FC_NVS_KEY_CFG, &stored, &len) == ESP_OK && len == sizeof(stored)) {
        *settings = stored;
    }
    nvs_close(h);
}

void fc_store_settings_save(const fc_settings_t *settings)
{
    if (!settings) return;
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (!h) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    if (nvs_set_blob(h, FC_NVS_KEY_CFG, settings, sizeof(*settings)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

void fc_store_history_append(uint32_t ymd, uint16_t reps, uint16_t minutes)
{
    fc_history_entry_t hist[FC_HISTORY_MAX];
    size_t count = fc_store_history_get(hist, FC_HISTORY_MAX);

    fc_history_entry_t newest = {ymd, reps, minutes};
    /* 同日同时长合并策略不搞：逐组记录，环覆盖旧的。 */
    if (count < FC_HISTORY_MAX) {
        for (size_t i = count; i > 0; i--) hist[i] = hist[i - 1];
        hist[0] = newest;
        count++;
    } else {
        for (size_t i = FC_HISTORY_MAX - 1; i > 0; i--) hist[i] = hist[i - 1];
        hist[0] = newest;
    }

    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (!h) return;
    if (nvs_set_blob(h, FC_NVS_KEY_HIST, hist, count * sizeof(hist[0])) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

size_t fc_store_history_get(fc_history_entry_t *out, size_t capacity)
{
    if (!out || capacity == 0) return 0;
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (!h) return 0;
    fc_history_entry_t hist[FC_HISTORY_MAX];
    size_t len = sizeof(hist);
    size_t count = 0;
    if (nvs_get_blob(h, FC_NVS_KEY_HIST, hist, &len) == ESP_OK) {
        count = len / sizeof(hist[0]);
        if (count > capacity) count = capacity;
        memcpy(out, hist, count * sizeof(hist[0]));
    }
    nvs_close(h);
    return count;
}

uint32_t fc_store_history_today_reps(uint32_t ymd)
{
    fc_history_entry_t hist[FC_HISTORY_MAX];
    const size_t count = fc_store_history_get(hist, FC_HISTORY_MAX);
    uint32_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (hist[i].ymd == ymd) total += hist[i].reps;
    }
    return total;
}

void fc_store_clear_network(void)
{
    fc_settings_t settings;
    fc_store_settings_load(&settings);
    settings.server_url[0] = '\0';
    settings.vlm_base_url[0] = '\0';
    settings.vlm_model[0] = '\0';
    settings.vlm_key[0] = '\0';
    settings.wifi_ssid[0] = '\0';
    settings.wifi_pass[0] = '\0';
    settings.mode = FC_MODE_DEMO;
    fc_store_settings_save(&settings);
}
