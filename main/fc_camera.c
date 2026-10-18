// SPDX-License-Identifier: Apache-2.0
// main/fc_camera.c —— 摄像头取帧 + 硬件 JPEG 编码。
//
// 链路与 BSP 例程 wifi_http_cam 一致：UYVY 帧直接走 esp32s31 JPEG 编码外设
// （JPEG_ENCODE_IN_FORMAT_YUV422），不做软件转换。
#include "fc_camera.h"

#include <inttypes.h>
#include <string.h>

#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fc_camera";

#define FC_JPEG_QUALITY 80
#define FC_JPEG_TIMEOUT_MS 1000

typedef struct {
    mosaico_camera_handle_t camera;
    jpeg_encoder_handle_t encoder;
    uint8_t *jpeg_buffer;
    size_t jpeg_capacity;
    bool streaming;
} fc_camera_t;

static fc_camera_t s_cam;

static esp_err_t open_and_stream(void)
{
    const mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    for (;;) {
        esp_err_t err = mosaico_camera_new(&config, &s_cam.camera);
        if (err == ESP_OK) err = mosaico_camera_open(s_cam.camera);
        mosaico_camera_info_t info = {0};
        if (err == ESP_OK) err = mosaico_camera_get_info(s_cam.camera, &info);
        if (err == ESP_OK && info.pixel_format != V4L2_PIX_FMT_UYVY) {
            ESP_LOGE(TAG, "unsupported format 0x%08" PRIx32, info.pixel_format);
            err = ESP_ERR_NOT_SUPPORTED;
        }
        if (err == ESP_OK && !s_cam.jpeg_buffer) {
            const jpeg_encode_memory_alloc_cfg_t memory = {
                .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
            };
            s_cam.jpeg_buffer = jpeg_alloc_encoder_mem(info.frame_buffer_size, &memory,
                                                       &s_cam.jpeg_capacity);
            if (!s_cam.jpeg_buffer) err = ESP_ERR_NO_MEM;
        }
        if (err == ESP_OK && !s_cam.encoder) {
            const jpeg_encode_engine_cfg_t engine = {.timeout_ms = FC_JPEG_TIMEOUT_MS};
            err = jpeg_new_encoder_engine(&engine, &s_cam.encoder);
        }
        if (err == ESP_OK) err = mosaico_camera_start_stream(s_cam.camera);
        if (err == ESP_OK) {
            s_cam.streaming = true;
            ESP_LOGI(TAG, "camera streaming %" PRIu32 "x%" PRIu32,
                     info.width, info.height);
            return ESP_OK;
        }
        /* 失败清理后重试（模块可能刚插上/上电中）。 */
        if (s_cam.encoder) {
            jpeg_del_encoder_engine(s_cam.encoder);
            s_cam.encoder = NULL;
        }
        if (s_cam.camera) {
            mosaico_camera_del(s_cam.camera);
            s_cam.camera = NULL;
        }
        ESP_LOGW(TAG, "open camera failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t fc_camera_start(void)
{
    if (s_cam.streaming) return ESP_OK;
    if (s_cam.camera && mosaico_camera_is_power_down(s_cam.camera)) {
        mosaico_camera_set_power_down(s_cam.camera, false);
    }
    return open_and_stream();
}

esp_err_t fc_camera_stop(void)
{
    if (!s_cam.streaming) return ESP_OK;
    s_cam.streaming = false;
    esp_err_t err = mosaico_camera_stop_stream(s_cam.camera);
    /* 保持打开但下电，减少静态功耗；下次 start 重新上电流。 */
    if (err == ESP_OK) err = mosaico_camera_set_power_down(s_cam.camera, true);
    return err;
}

bool fc_camera_is_streaming(void)
{
    return s_cam.streaming;
}

esp_err_t fc_camera_get_frame(mosaico_camera_frame_t *out_frame)
{
    if (!out_frame || !s_cam.streaming) return ESP_ERR_INVALID_STATE;
    return mosaico_camera_get_frame(s_cam.camera, out_frame);
}

esp_err_t fc_camera_put_frame(const mosaico_camera_frame_t *frame)
{
    if (!frame || !s_cam.camera) return ESP_ERR_INVALID_ARG;
    return mosaico_camera_return_frame(s_cam.camera, frame);
}

esp_err_t fc_camera_encode_jpeg(const mosaico_camera_frame_t *frame,
                                const uint8_t **out_jpeg, size_t *out_len)
{
    if (!frame || !out_jpeg || !out_len || !s_cam.encoder) return ESP_ERR_INVALID_STATE;
    if (frame->pixel_format != V4L2_PIX_FMT_UYVY || frame->size == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const jpeg_encode_cfg_t cfg = {
        .height = frame->height,
        .width = frame->width,
        .src_type = JPEG_ENCODE_IN_FORMAT_YUV422,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = FC_JPEG_QUALITY,
    };
    uint32_t encoded = 0;
    esp_err_t err = jpeg_encoder_process(s_cam.encoder, &cfg, frame->data, frame->size,
                                         s_cam.jpeg_buffer, s_cam.jpeg_capacity,
                                         &encoded);
    if (err != ESP_OK) return err;
    *out_jpeg = s_cam.jpeg_buffer;
    *out_len = encoded;
    return ESP_OK;
}
