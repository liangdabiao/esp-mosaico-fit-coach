// SPDX-License-Identifier: Apache-2.0
// main/fc_provider_http.c —— MediaPipe 服务器 provider（主路径）。
//
// POST <server_url>/frame（body = JPEG），响应 JSON：
//   {"angle_deg": 97.5, "stage": "up", "visible": true, "t_ms": 1234}
// t_ms 回显请求头 X-FC-TS 携带的采集时间戳；设备忽略服务器时钟。
#include "fc_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "fc_prov_http";

#define FC_HTTP_TIMEOUT_MS 5000
#define FC_HTTP_RESP_MAX 1024

typedef struct {
    fc_provider_t base;
    fc_provider_cbs_t cbs;
    char url[160]; /* <server_url>/frame */
    esp_http_client_handle_t client;
    char *resp;
} http_provider_t;

static esp_err_t dispatch_sample(http_provider_t *p, const char *body, uint32_t t_ms)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "bad json: %.80s", body);
        return ESP_ERR_INVALID_STATE;
    }
    fc_sample_t s = {
        .angle_deg = -1.0f,
        .stage = FC_STAGE_UNKNOWN,
        .visible = false,
        .conf = 1.0f,
        .t_ms = t_ms,
    };
    const cJSON *angle = cJSON_GetObjectItem(root, "angle_deg");
    if (cJSON_IsNumber(angle) && angle->valuedouble >= 0.0) {
        s.angle_deg = (float)angle->valuedouble;
    }
    const cJSON *stage = cJSON_GetObjectItem(root, "stage");
    if (cJSON_IsString(stage) && stage->valuestring) {
        if (strcmp(stage->valuestring, "up") == 0) s.stage = FC_STAGE_UP;
        else if (strcmp(stage->valuestring, "down") == 0) s.stage = FC_STAGE_DOWN;
    }
    const cJSON *visible = cJSON_GetObjectItem(root, "visible");
    s.visible = cJSON_IsTrue(visible);
    const cJSON *conf = cJSON_GetObjectItem(root, "conf");
    if (cJSON_IsNumber(conf)) s.conf = (float)conf->valuedouble;
    cJSON_Delete(root);

    if (p->cbs.on_sample) p->cbs.on_sample(&s, p->cbs.ctx);
    return ESP_OK;
}

static esp_err_t http_feed(fc_provider_t *provider, const uint8_t *jpeg, size_t len,
                           uint32_t t_ms)
{
    http_provider_t *p = (http_provider_t *)provider;
    if (!p || !jpeg || !len) return ESP_ERR_INVALID_ARG;

    char ts[16];
    snprintf(ts, sizeof(ts), "%lu", (unsigned long)t_ms);
    esp_http_client_set_method(p->client, HTTP_METHOD_POST);
    esp_http_client_set_header(p->client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(p->client, "X-FC-TS", ts);
    esp_http_client_set_post_field(p->client, (const char *)jpeg, (int)len);

    esp_err_t err = esp_http_client_open(p->client, (int)len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        goto fail;
    }
    int written = esp_http_client_write(p->client, (const char *)jpeg, (int)len);
    if (written != (int)len) {
        esp_http_client_close(p->client);
        err = ESP_ERR_HTTP_WRITE_DATA;
        goto fail;
    }
    const int64_t hlen = esp_http_client_fetch_headers(p->client);
    if (hlen < 0) {
        esp_http_client_close(p->client);
        err = ESP_ERR_HTTP_FETCH_HEADER;
        goto fail;
    }
    int code = esp_http_client_get_status_code(p->client);
    int got = 0;
    if (code == 200) {
        got = esp_http_client_read_response(p->client, p->resp, FC_HTTP_RESP_MAX - 1);
    }
    esp_http_client_close(p->client);
    if (code != 200 || got <= 0) {
        ESP_LOGW(TAG, "server code=%d got=%d", code, got);
        err = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    p->resp[got] = '\0';
    err = dispatch_sample(p, p->resp, t_ms);
    if (err == ESP_OK && p->cbs.on_status) p->cbs.on_status(true, p->cbs.ctx);
    return err;

fail:
    if (p->cbs.on_status) p->cbs.on_status(false, p->cbs.ctx);
    return err;
}

static void http_del(fc_provider_t *provider)
{
    http_provider_t *p = (http_provider_t *)provider;
    if (!p) return;
    if (p->client) esp_http_client_cleanup(p->client);
    free(p->resp);
    free(p);
}

fc_provider_t *fc_provider_http_new(const char *server_url, const fc_provider_cbs_t *cbs)
{
    if (!server_url || !server_url[0] || !cbs) return NULL;
    http_provider_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->base.name = "http";
    p->base.feed = http_feed;
    p->base.del = http_del;
    p->cbs = *cbs;
    snprintf(p->url, sizeof(p->url), "%s/frame", server_url);
    /* 去掉可能拼接出的双斜杠之外的问题：只处理结尾斜杠。 */
    p->resp = malloc(FC_HTTP_RESP_MAX);
    if (!p->resp) {
        free(p);
        return NULL;
    }
    esp_http_client_config_t cfg = {
        .url = p->url,
        .timeout_ms = FC_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .buffer_size = 2048,
    };
    p->client = esp_http_client_init(&cfg);
    if (!p->client) {
        free(p->resp);
        free(p);
        return NULL;
    }
    return &p->base;
}
