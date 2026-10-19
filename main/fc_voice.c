// SPDX-License-Identifier: Apache-2.0
// main/fc_voice.c —— 设备端语音播放器：ADPCM blob → 流式解码 → ES8311。
//
// blob 由 tools/fit_coach/gen_voice.py 生成（FCVA 格式：16B 头 + 每片段 8B
// 索引 + IMA-ADPCM 载荷），经 target_add_binary_data 编进固件 rodata。
// 与 rtttl_player 共用 16kHz I2S：播放前等提示音结束（互斥，不混音）。
#include "fc_voice.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "bsp/audio.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "fc_adpcm.h"
#include "fc_voice_clips.h"
#include "rtttl_player.h"

static const char *TAG = "fc_voice";

/* 链接器符号：blob 以 _binary_<文件名>_start/end 编入。 */
extern const uint8_t fc_voice_bin_start[] asm("_binary_fc_voice_bin_start");
extern const uint8_t fc_voice_bin_end[] asm("_binary_fc_voice_bin_end");

#define FC_VOICE_QUEUE_LEN 6
#define FC_VOICE_CHUNK_SAMPLES 1024
#define FC_VOICE_VOLUME 70

/* blob 布局（与 gen_voice.py 的 render_blob 逐字段对应）。 */
#define BLOB_HEADER_BYTES 16
#define BLOB_INDEX_ENTRY_BYTES 8

typedef struct {
    uint32_t offset;
    uint32_t sample_count;
} clip_entry_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static esp_codec_dev_handle_t s_speaker;
static bool s_running;

static uint16_t read_u16(const uint8_t *at)
{
    return (uint16_t)((uint16_t)at[0] | ((uint16_t)at[1] << 8));
}

static uint32_t read_u32(const uint8_t *at)
{
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
           ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

/* 校验并取片段。返回 false 说明 blob 与固件不配套（刷错包/截断）。 */
static bool lookup_clip(int clip_id, clip_entry_t *out)
{
    const uint8_t *blob = fc_voice_bin_start;
    const uint32_t length = (uint32_t)(fc_voice_bin_end - fc_voice_bin_start);
    if (length < BLOB_HEADER_BYTES) return false;
    if (blob[0] != 'F' || blob[1] != 'C' || blob[2] != 'V' || blob[3] != 'A') return false;
    if (read_u16(blob + 4) != 1) return false;
    const uint16_t clip_count = read_u16(blob + 6);
    if (read_u32(blob + 8) != FC_VOICE_SAMPLE_RATE) return false;
    if (clip_count != FC_VOICE_CLIP_COUNT) return false;
    if (clip_id < 0 || clip_id >= clip_count) return false;

    const uint8_t *entry = blob + BLOB_HEADER_BYTES +
                           (size_t)clip_id * BLOB_INDEX_ENTRY_BYTES;
    const uint32_t offset = read_u32(entry);
    const uint32_t samples = read_u32(entry + 4);
    if (samples == 0) return false;
    if ((uint64_t)offset + fc_adpcm_encoded_size(samples) > (uint64_t)length) {
        return false;
    }
    out->offset = offset;
    out->sample_count = samples;
    return true;
}

static void play_clip(int clip_id)
{
    clip_entry_t entry;
    if (!lookup_clip(clip_id, &entry)) {
        ESP_LOGW(TAG, "clip %d not found", clip_id);
        return;
    }

    /* 与 rtttl 互斥：提示音最长几百毫秒，等它结束再开口。 */
    for (int wait = 0; wait < 10 && rtttl_player_is_playing(); wait++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    int16_t *pcm = malloc(FC_VOICE_CHUNK_SAMPLES * sizeof(int16_t));
    if (!pcm) return;

    fc_adpcm_stream_t stream;
    fc_adpcm_stream_open(&stream, fc_voice_bin_start + entry.offset,
                         entry.sample_count);
    esp_codec_dev_set_out_vol(s_speaker, FC_VOICE_VOLUME);
    for (;;) {
        const uint32_t got = fc_adpcm_stream_read(&stream, pcm, FC_VOICE_CHUNK_SAMPLES);
        if (got == 0) break;
        if (esp_codec_dev_write(s_speaker, pcm, got * sizeof(int16_t)) != ESP_OK) {
            break;
        }
        if (!s_running) break; /* 停机时立即收声 */
    }
    free(pcm);
}

static void voice_task(void *arg)
{
    (void)arg;
    int clip_id;
    while (s_running) {
        if (xQueueReceive(s_queue, &clip_id, portMAX_DELAY) != pdTRUE) continue;
        if (!s_running) break;
        play_clip(clip_id);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

int fc_voice_start(bool codec_already_open)
{
    if (s_running) return 0;

    /* 校验 blob：启动即发现不配套，而不是第一次开口时。 */
    clip_entry_t probe;
    if (!lookup_clip(0, &probe)) {
        ESP_LOGE(TAG, "voice blob invalid; voice disabled");
        return -1;
    }

    s_speaker = bsp_audio_codec_speaker_init();
    if (!s_speaker) return -1;
    /* rtttl_player_start 已按 16kHz 打开过 codec（同一句柄）；只有提示音
     * 没起来时才由这里补开。 */
    if (!codec_already_open) {
        esp_codec_dev_sample_info_t info = {
            .sample_rate = FC_VOICE_SAMPLE_RATE,
            .channel = 1,
            .bits_per_sample = 16,
        };
        if (esp_codec_dev_open(s_speaker, &info) != ESP_OK) return -1;
    }

    s_queue = xQueueCreate(FC_VOICE_QUEUE_LEN, sizeof(int));
    if (!s_queue) return -1;
    s_running = true;
    if (xTaskCreate(voice_task, "fc_voice", 4096, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return -1;
    }
    ESP_LOGI(TAG, "voice ready: %d clips", FC_VOICE_CLIP_COUNT);
    return 0;
}

void fc_voice_stop(void)
{
    if (!s_running) return;
    s_running = false;
    fc_voice_flush();
    if (s_queue) {
        /* 唤醒任务使其退出。 */
        int done = -1;
        xQueueSend(s_queue, &done, 0);
    }
    while (s_task) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
}

bool fc_voice_available(void)
{
    return s_running;
}

static void enqueue(int clip_id)
{
    if (!s_running || clip_id < 0 || clip_id >= FC_VOICE_CLIP_COUNT) return;
    if (uxQueueSpacesAvailable(s_queue) == 0) {
        int dropped;
        xQueueReceive(s_queue, &dropped, 0); /* 满则丢最旧 */
    }
    xQueueSend(s_queue, &clip_id, 0);
}

void fc_voice_play(int clip_id)
{
    enqueue(clip_id);
}

void fc_voice_play2(int a, int b)
{
    enqueue(a);
    enqueue(b);
}

void fc_voice_play3(int a, int b, int c)
{
    enqueue(a);
    enqueue(b);
    enqueue(c);
}

void fc_voice_flush(void)
{
    if (!s_queue) return;
    xQueueReset(s_queue);
}
