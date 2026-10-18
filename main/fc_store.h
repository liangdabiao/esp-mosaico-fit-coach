// SPDX-License-Identifier: Apache-2.0
// main/fc_store.h —— 设置与会话历史的持久化接口（设备 NVS / 宿主内存双实现）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 模式：演示（合成角度）/ MediaPipe 服务器 / VLM 直连。 */
#define FC_MODE_DEMO 0
#define FC_MODE_HTTP 1
#define FC_MODE_VLM 2

#define FC_URL_MAX 96
/* 与 esp_wifi 的 sta.ssid(32)/sta.password(64) 严格对齐，避免截断告警。 */
#define FC_SSID_MAX 32
#define FC_PASS_MAX 64
#define FC_MODEL_MAX 32
#define FC_KEY_MAX 72

typedef struct {
    uint8_t mode;              /* FC_MODE_* */
    uint8_t target_reps;       /* 每组目标个数 */
    uint8_t sound_on;          /* 1 开 / 0 关 */
    int16_t up_thresh;         /* 躺平阈值（度） */
    int16_t down_thresh;       /* 坐起阈值（度） */
    char server_url[FC_URL_MAX];   /* MediaPipe 服务器，如 http://192.168.1.10:8000 */
    char vlm_base_url[FC_URL_MAX]; /* OpenAI 兼容端点，如 https://api.xxx.com/v1 */
    char vlm_model[FC_MODEL_MAX];
    char vlm_key[FC_KEY_MAX];
    char wifi_ssid[FC_SSID_MAX];
    char wifi_pass[FC_PASS_MAX];
} fc_settings_t;

typedef struct {
    uint32_t ymd;    /* 会话日期 yyyymmdd；未知时钟时为 0 */
    uint16_t reps;   /* 该组次数 */
    uint16_t minutes; /* 该组时长（分钟） */
} fc_history_entry_t;

#define FC_HISTORY_MAX 16

void fc_settings_defaults(fc_settings_t *settings);

/* 载入（无记录时写默认值）/保存设置。宿主实现为进程内静态区。 */
void fc_store_settings_load(fc_settings_t *settings);
void fc_store_settings_save(const fc_settings_t *settings);

/* 追加一条会话记录（环写，最多 FC_HISTORY_MAX 条）。 */
void fc_store_history_append(uint32_t ymd, uint16_t reps, uint16_t minutes);

/* 读历史（最新在前），返回实际拷贝条数。 */
size_t fc_store_history_get(fc_history_entry_t *out, size_t capacity);

/* 汇总：今天的总次数（ymd 匹配）。 */
uint32_t fc_store_history_today_reps(uint32_t ymd);

/* 只清网络相关配置（WiFi/服务器/VLM），其余保留；用于设置页「重置网络」。 */
void fc_store_clear_network(void);

#ifdef __cplusplus
}
#endif
