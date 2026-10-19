// SPDX-License-Identifier: Apache-2.0
// main/fc_net.c —— WiFi STA + SoftAP 配网门户 + SNTP。
#include "fc_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "fc_store.h"

static const char *TAG = "fc_net";

#define FC_AP_SSID_PREFIX "fitcoach-"
#define FC_AP_CHANNEL 6
#define FC_STA_CONNECT_TIMEOUT_MS 30000
#define FC_NET_EVT_GOT_IP BIT0

static esp_netif_t *s_sta_netif;
static httpd_handle_t s_portal;
static EventGroupHandle_t s_events;
static volatile fc_net_state_t s_state = FC_NET_DOWN;
static bool s_started;

/* ---------------- 配网门户 ---------------- */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>fit-coach</title><style>"
    "body{font-family:sans-serif;background:#10121a;color:#e2e6f0;margin:0;padding:24px}"
    "h1{font-size:20px}label{display:block;margin:14px 0 4px;font-size:14px;color:#8890a2}"
    "input{width:100%;box-sizing:border-box;padding:10px;border:1px solid #464e64;"
    "border-radius:6px;background:#1a1e2a;color:#e2e6f0;font-size:15px}"
    "button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:6px;"
    "background:#6ea0ff;color:#10121a;font-size:16px;font-weight:bold}"
    "small{color:#6a7288}</style></head><body>"
    "<h1>fit-coach 姿态健身教练 · 配网</h1>"
    "<form method=\"POST\" action=\"/save\">"
    "<label>WiFi 名称 (SSID)</label><input name=\"ssid\" required maxlength=\"32\">"
    "<label>WiFi 密码</label><input name=\"pass\" type=\"password\" maxlength=\"64\">"
    "<label>MediaPipe 服务器地址（主模式，如 http://192.168.1.10:8000）</label>"
    "<input name=\"server\" placeholder=\"http://ip:port\">"
    "<label>视觉模型端点（备用模式，OpenAI 兼容，可留空）</label>"
    "<input name=\"vlmurl\" placeholder=\"https://host/v1\">"
    "<label>视觉模型名（如 glm-4v-flash）</label><input name=\"vlmmodel\">"
    "<label>视觉模型 API Key</label><input name=\"vlmkey\" type=\"password\">"
    "<button type=\"submit\">保存并重启</button>"
    "</form><small>保存后设备将重启并连接 WiFi。</small></body></html>";

/* 解码 application/x-www-form-urlencoded（含 + 与 %XX）。 */
static void url_decode(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *s = src; *s && o + 1 < cap; s++) {
        if (*s == '+') {
            dst[o++] = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            dst[o++] = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            dst[o++] = *s;
        }
    }
    dst[o] = '\0';
}

/* 从 POST body 提取字段。 */
static void form_field(const char *body, const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", name);
    const char *p = strstr(body, pattern);
    if (!p) return;
    p += strlen(pattern);
    const char *end = strchr(p, '&');
    char raw[FC_URL_MAX + FC_KEY_MAX];
    const size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n >= sizeof(raw)) return;
    memcpy(raw, p, n);
    raw[n] = '\0';
    url_decode(raw, out, cap);
}

static esp_err_t portal_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_save(httpd_req_t *req)
{
    char body[1024];
    size_t total = req->content_len > sizeof(body) - 1 ? sizeof(body) - 1 : req->content_len;
    int received = httpd_req_recv(req, body, total);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    fc_settings_t settings;
    fc_store_settings_load(&settings);
    form_field(body, "ssid", settings.wifi_ssid, sizeof(settings.wifi_ssid));
    form_field(body, "pass", settings.wifi_pass, sizeof(settings.wifi_pass));
    form_field(body, "server", settings.server_url, sizeof(settings.server_url));
    form_field(body, "vlmurl", settings.vlm_base_url, sizeof(settings.vlm_base_url));
    form_field(body, "vlmmodel", settings.vlm_model, sizeof(settings.vlm_model));
    form_field(body, "vlmkey", settings.vlm_key, sizeof(settings.vlm_key));
    /* 有服务器地址默认走服务器模式；否则有 VLM 配置走 VLM；再否则演示。 */
    if (settings.server_url[0]) settings.mode = FC_MODE_HTTP;
    else if (settings.vlm_base_url[0] && settings.vlm_model[0]) settings.mode = FC_MODE_VLM;
    else settings.mode = FC_MODE_DEMO;
    fc_store_settings_save(&settings);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, "<!DOCTYPE html><html><body style=\"font-family:sans-serif;"
                         "background:#10121a;color:#e2e6f0;text-align:center;padding:40px\">"
                         "<h2>已保存，设备即将重启</h2></body></html>",
                    HTTPD_RESP_USE_STRLEN);
    /* 等响应送达再重启。 */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK; /* 不会到达 */
}

static void start_portal(void)
{
    if (s_portal) return;
    ESP_LOGI(TAG, "starting provisioning portal");

    /* 网络失败绝不 abort（曾在引擎/NVS 就绪前初始化 WiFi 导致启动
     * abort→回滚 recovery，core dump 定位于此）：全部软失败，只记日志。 */
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal wifi init failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t wifi = {0};
    snprintf((char *)wifi.ap.ssid, sizeof(wifi.ap.ssid), FC_AP_SSID_PREFIX "%02X%02X",
             mac[4], mac[5]);
    wifi.ap.ssid_len = strlen((char *)wifi.ap.ssid);
    wifi.ap.channel = FC_AP_CHANNEL;
    wifi.ap.max_connection = 2;
    wifi.ap.authmode = WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal wifi start failed: %s", esp_err_to_name(err));
        return;
    }

    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_portal, &http) == ESP_OK) {
        httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = portal_index};
        httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = portal_save};
        httpd_register_uri_handler(s_portal, &index);
        httpd_register_uri_handler(s_portal, &save);
    }
    ESP_LOGW(TAG, "connect to AP \"%s\" then open http://192.168.4.1/",
             (const char *)wifi.ap.ssid);
    s_state = FC_NET_DOWN;
}

/* ---------------- STA ---------------- */

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_state = FC_NET_READY;
        if (s_events) xEventGroupSetBits(s_events, FC_NET_EVT_GOT_IP);
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "sta got ip " IPSTR, IP2STR(&evt->ip_info.ip));
        /* 对时（异步；失败不影响主流程，历史记为无日期）。 */
        {
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sntp);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == FC_NET_READY) s_state = FC_NET_CONNECTING;
        esp_wifi_connect(); /* 自动重连 */
    }
}

static void sta_connect(const fc_settings_t *settings)
{
    s_state = FC_NET_CONNECTING;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* 软失败：任何一步失败都回退门户/演示，绝不 abort。 */
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                         on_ip_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         on_ip_event, NULL);
    }
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        wifi_config_t wifi = {0};
        snprintf((char *)wifi.sta.ssid, sizeof(wifi.sta.ssid), "%s", settings->wifi_ssid);
        snprintf((char *)wifi.sta.password, sizeof(wifi.sta.password), "%s",
                 settings->wifi_pass);
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi);
        if (err == ESP_OK) err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sta wifi init failed: %s", esp_err_to_name(err));
        start_portal();
        return;
    }
    esp_wifi_connect();

    /* 30 秒拿不到 IP → 开门户让用户重配。 */
    const EventBits_t bits = xEventGroupWaitBits(s_events, FC_NET_EVT_GOT_IP,
                                                 pdTRUE, pdFALSE,
                                                 pdMS_TO_TICKS(FC_STA_CONNECT_TIMEOUT_MS));
    if (bits & FC_NET_EVT_GOT_IP) return;
    ESP_LOGW(TAG, "sta connect timeout; falling back to portal");
    /* 切回 AP 门户（重启法最干净：清空 STA 侧状态）。 */
    esp_wifi_stop();
    esp_wifi_deinit();
    start_portal();
}

/* ---------------- 对外接口 ---------------- */

esp_err_t fc_net_start(void)
{
    if (s_started) return ESP_OK;
    s_started = true;

    /* WiFi 依赖 NVS（校准数据）；引擎可能已初始化，这里幂等补一枪，
     * 擦除-重试只在首次分区升级场景出现。 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs erase-retry: %s", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s (demo mode continues)", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
        return err;
    }
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    fc_settings_t settings;
    fc_store_settings_load(&settings);
    if (settings.wifi_ssid[0]) {
        sta_connect(&settings);
    } else {
        start_portal();
    }
    return ESP_OK;
}

void fc_net_stop(void)
{
    if (s_portal) {
        httpd_stop(s_portal);
        s_portal = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_events) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    s_started = false;
    s_state = FC_NET_DOWN;
}

bool fc_net_sta_ready(void)
{
    return s_state == FC_NET_READY;
}

fc_net_state_t fc_net_state(void)
{
    return s_state;
}
