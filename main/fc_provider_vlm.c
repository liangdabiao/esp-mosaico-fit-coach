// SPDX-License-Identifier: Apache-2.0
// main/fc_provider_vlm.c —— 视觉大模型 provider（备用路径，OpenAI 兼容）。
//
// 4 帧组批：runtime 以 1fps 送帧，这里攒满 4 帧后发一次 chat/completions，
// user content = [提示文本, image_url x4]（按时间顺序）。提示词要求只回
// 紧凑 JSON：{"states":["lying","sitting",...],"conf":0.9}。
// lying→躺平(UP)、sitting→坐起(DOWN)、其余→UNKNOWN（内核去抖后丢弃）。
// 网络往返慢（1~3s）不影响计数：帧的采集时刻随帧携带。
#include "fc_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#include "fc_store.h"

static const char *TAG = "fc_prov_vlm";

#define FC_VLM_BATCH 4
#define FC_VLM_TIMEOUT_MS 15000
#define FC_VLM_RESP_MAX 2048

/* 覆盖：躺/坐状态判定 + 输出格式约束。中英混合以适配多模型。 */
static const char *FC_VLM_PROMPT =
    "These are " "4 consecutive video frames of one person doing sit-ups, in "
    "chronological order. For EACH frame classify the person's torso state: "
    "\"lying\" (lying flat, torso extended) or \"sitting\" (curled up) or "
    "\"unclear\" (no person or ambiguous). Reply with ONLY compact JSON, no "
    "markdown: {\"states\":[\"..\",\"..\",\"..\",\"..\"],\"conf\":0.0-1.0}";

typedef struct {
    fc_provider_t base;
    fc_provider_cbs_t cbs;
    char url[160]; /* <base_url>/chat/completions */
    char auth[FC_KEY_MAX + 16];
    char model[FC_MODEL_MAX];
    uint8_t *jpegs[FC_VLM_BATCH];
    size_t jpeg_lens[FC_VLM_BATCH];
    uint32_t jpeg_ts[FC_VLM_BATCH];
    uint8_t batch_count;
    char *body;    /* PSRAM，请求体 */
    size_t body_cap;
    char *resp;
} vlm_provider_t;

static void free_batch(vlm_provider_t *p)
{
    for (int i = 0; i < p->batch_count; i++) {
        free(p->jpegs[i]);
        p->jpegs[i] = NULL;
        p->jpeg_lens[i] = 0;
    }
    p->batch_count = 0;
}

static bool body_reserve(vlm_provider_t *p, size_t need)
{
    if (p->body && p->body_cap >= need) return true;
    free(p->body);
    /* 4 图 base64 + JSON 骨架；PSRAM（内部 RAM 放不下且易碎片化）。 */
    p->body_cap = need + 4096;
    p->body = heap_caps_malloc(p->body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p->body != NULL;
}

/* 追加写入指针。 */
typedef struct {
    char *p;
    size_t left;
} wr_t;

static void wr_puts(wr_t *w, const char *s)
{
    const size_t n = strlen(s);
    if (n >= w->left) {
        w->left = 0;
        return;
    }
    memcpy(w->p, s, n);
    w->p += n;
    w->left -= n;
}

static void wr_json_escape(wr_t *w, const char *s)
{
    for (; *s && w->left; s++) {
        char buf[8] = {0};
        if ((unsigned char)*s < 0x20 || *s == '\"' || *s == '\\') {
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)*s);
            wr_puts(w, buf);
        } else {
            *w->p++ = *s;
            w->left--;
        }
    }
}

static esp_err_t build_body(vlm_provider_t *p, const char *model)
{
    /* 计算容量：base64 ≈ 4/3。 */
    size_t b64_total = 0;
    for (int i = 0; i < p->batch_count; i++) {
        b64_total += p->jpeg_lens[i] * 4u / 3u + 8u;
    }
    if (!body_reserve(p, b64_total + 1024)) return ESP_ERR_NO_MEM;

    wr_t w = {p->body, p->body_cap};
    wr_puts(&w, "{\"model\":\"");
    wr_json_escape(&w, model);
    wr_puts(&w, "\",\"max_tokens\":200,\"messages\":[{\"role\":\"user\",\"content\":[");
    wr_puts(&w, "{\"type\":\"text\",\"text\":\"");
    wr_json_escape(&w, FC_VLM_PROMPT);
    wr_puts(&w, "\"}");
    for (int i = 0; i < p->batch_count && w.left; i++) {
        wr_puts(&w, ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,");
        /* 分块 base64，直接写进 body。 */
        const uint8_t *src = p->jpegs[i];
        size_t off = 0;
        while (off < p->jpeg_lens[i] && w.left > 8) {
            size_t chunk = p->jpeg_lens[i] - off;
            if (chunk > 3072) chunk = 3072;
            size_t olen = 0;
            const int ret = mbedtls_base64_encode((unsigned char *)w.p, w.left - 1,
                                                  &olen, &src[off], chunk);
            if (ret != 0) return ESP_ERR_NO_MEM;
            w.p += olen;
            w.left -= olen;
            off += chunk;
        }
        wr_puts(&w, "\"}}");
    }
    wr_puts(&w, "]}]}");
    if (w.left == 0) return ESP_ERR_NO_MEM;
    *w.p = '\0';
    return ESP_OK;
}

static fc_stage_t stage_from_label(const char *label)
{
    if (strcmp(label, "lying") == 0) return FC_STAGE_UP;
    if (strcmp(label, "sitting") == 0) return FC_STAGE_DOWN;
    return FC_STAGE_UNKNOWN;
}

static esp_err_t dispatch_batch(vlm_provider_t *p)
{
    if (p->batch_count == 0) return ESP_OK;

    /* 逐帧样本：states 数组与帧一一对应（缺失按 unclear 处理）。 */
    char labels[FC_VLM_BATCH][16] = {{0}};
    float conf = 0.8f;
    cJSON *root = cJSON_Parse(p->resp);
    if (!root) {
        ESP_LOGW(TAG, "bad json: %.80s", p->resp);
        return ESP_ERR_INVALID_STATE; /* 批次由 send_batch 的失败路径统一释放 */
    }
    const cJSON *choices = cJSON_GetObjectItem(root, "choices");
    const cJSON *first = cJSON_GetArrayItem(choices, 0);
    const cJSON *message = first ? cJSON_GetObjectItem(first, "message") : NULL;
    const cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;
    if (cJSON_IsString(content) && content->valuestring) {
        /* content 里应当只有 JSON；剥掉可能的 markdown 围栏。 */
        const char *c = content->valuestring;
        char clean[FC_VLM_RESP_MAX];
        snprintf(clean, sizeof(clean), "%s", c);
        char *l = strstr(clean, "```");
        if (l) {
            char *start = strchr(l + 3, '\n');
            char *end = start ? strstr(start, "```") : NULL;
            if (start && end) {
                *end = '\0';
                memmove(clean, start + 1, strlen(start + 1) + 1);
            }
        }
        cJSON *inner = cJSON_Parse(clean);
        if (inner) {
            const cJSON *states = cJSON_GetObjectItem(inner, "states");
            const cJSON *it = states ? states->child : NULL;
            for (int i = 0; i < FC_VLM_BATCH && it; i++, it = it->next) {
                if (cJSON_IsString(it) && it->valuestring) {
                    snprintf(labels[i], sizeof(labels[i]), "%s", it->valuestring);
                }
            }
            const cJSON *jc = cJSON_GetObjectItem(inner, "conf");
            if (cJSON_IsNumber(jc)) conf = (float)jc->valuedouble;
            cJSON_Delete(inner);
        }
    }
    cJSON_Delete(root);

    for (int i = 0; i < p->batch_count; i++) {
        fc_sample_t s = {
            .angle_deg = -1.0f,
            .stage = stage_from_label(labels[i]),
            .visible = labels[i][0] != '\0' && strcmp(labels[i], "unclear") != 0,
            .conf = conf,
            .t_ms = p->jpeg_ts[i],
        };
        if (p->cbs.on_sample) p->cbs.on_sample(&s, p->cbs.ctx);
    }
    free_batch(p);
    if (p->cbs.on_status) p->cbs.on_status(true, p->cbs.ctx);
    return ESP_OK;
}

static esp_err_t send_batch(vlm_provider_t *p, const char *model)
{
    esp_err_t err = build_body(p, model);
    if (err != ESP_OK) {
        free_batch(p);
        return err;
    }

    esp_http_client_config_t cfg = {
        .url = p->url,
        .timeout_ms = FC_VLM_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free_batch(p);
        return ESP_FAIL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (p->auth[0]) esp_http_client_set_header(client, "Authorization", p->auth);

    const size_t len = strlen(p->body);
    esp_http_client_set_post_field(client, p->body, (int)len);
    err = esp_http_client_open(client, (int)len);
    if (err == ESP_OK) {
        const int written = esp_http_client_write(client, p->body, (int)len);
        if (written == (int)len) {
            const int64_t hlen = esp_http_client_fetch_headers(client);
            const int code = esp_http_client_get_status_code(client);
            int got = 0;
            if (code == 200 && hlen >= 0) {
                got = esp_http_client_read_response(client, p->resp, FC_VLM_RESP_MAX - 1);
            }
            esp_http_client_close(client);
            if (code == 200 && got > 0) {
                p->resp[got] = '\0';
                err = dispatch_batch(p);
            } else {
                ESP_LOGW(TAG, "vlm code=%d got=%d", code, got);
                err = ESP_ERR_INVALID_RESPONSE;
            }
        } else {
            esp_http_client_close(client);
            err = ESP_ERR_HTTP_WRITE_DATA;
        }
    } else {
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        free_batch(p);
        if (p->cbs.on_status) p->cbs.on_status(false, p->cbs.ctx);
    }
    return err;
}

static esp_err_t vlm_feed(fc_provider_t *provider, const uint8_t *jpeg, size_t len,
                          uint32_t t_ms)
{
    vlm_provider_t *p = (vlm_provider_t *)provider;
    if (!p || !jpeg || !len) return ESP_ERR_INVALID_ARG;

    if (p->batch_count < FC_VLM_BATCH) {
        uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!copy) return ESP_ERR_NO_MEM;
        memcpy(copy, jpeg, len);
        p->jpegs[p->batch_count] = copy;
        p->jpeg_lens[p->batch_count] = len;
        p->jpeg_ts[p->batch_count] = t_ms;
        p->batch_count++;
    }
    if (p->batch_count < FC_VLM_BATCH) return ESP_OK; /* 攒批中 */

    return send_batch(p, p->model);
}

static void vlm_del(fc_provider_t *provider)
{
    vlm_provider_t *p = (vlm_provider_t *)provider;
    if (!p) return;
    free_batch(p);
    free(p->body);
    free(p->resp);
    free(p);
}

fc_provider_t *fc_provider_vlm_new(const char *base_url, const char *model,
                                   const char *api_key, const fc_provider_cbs_t *cbs)
{
    if (!base_url || !base_url[0] || !model || !model[0] || !cbs) return NULL;
    vlm_provider_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->base.name = "vlm";
    p->base.feed = vlm_feed;
    p->base.del = vlm_del;
    p->cbs = *cbs;
    snprintf(p->url, sizeof(p->url), "%s/chat/completions", base_url);
    if (api_key && api_key[0]) snprintf(p->auth, sizeof(p->auth), "Bearer %s", api_key);
    snprintf(p->model, sizeof(p->model), "%s", model);
    p->resp = heap_caps_malloc(FC_VLM_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p->resp) {
        free(p);
        return NULL;
    }
    return &p->base;
}
