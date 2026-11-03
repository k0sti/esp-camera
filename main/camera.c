#include "camera.h"

#include <strings.h>

#include "esp_log.h"

#include "board_pins.h"

static const char *TAG = "camera";

static const char *const size_names[FRAMESIZE_INVALID] = {
    [FRAMESIZE_96X96] = "96X96",     [FRAMESIZE_QQVGA] = "QQVGA",
    [FRAMESIZE_128X128] = "128X128", [FRAMESIZE_QCIF] = "QCIF",
    [FRAMESIZE_HQVGA] = "HQVGA",     [FRAMESIZE_240X240] = "240X240",
    [FRAMESIZE_QVGA] = "QVGA",       [FRAMESIZE_320X320] = "320X320",
    [FRAMESIZE_CIF] = "CIF",         [FRAMESIZE_HVGA] = "HVGA",
    [FRAMESIZE_VGA] = "VGA",         [FRAMESIZE_SVGA] = "SVGA",
    [FRAMESIZE_XGA] = "XGA",         [FRAMESIZE_HD] = "HD",
    [FRAMESIZE_SXGA] = "SXGA",       [FRAMESIZE_UXGA] = "UXGA",
    [FRAMESIZE_FHD] = "FHD",         [FRAMESIZE_P_HD] = "P_HD",
    [FRAMESIZE_P_3MP] = "P_3MP",     [FRAMESIZE_QXGA] = "QXGA",
    [FRAMESIZE_QHD] = "QHD",         [FRAMESIZE_WQXGA] = "WQXGA",
    [FRAMESIZE_P_FHD] = "P_FHD",     [FRAMESIZE_QSXGA] = "QSXGA",
    [FRAMESIZE_5MP] = "5MP",
};

static framesize_t max_size = FRAMESIZE_INVALID;

framesize_t camera_max_size(void)
{
    return max_size;
}

const char *camera_size_name(framesize_t size)
{
    return size >= 0 && size < FRAMESIZE_INVALID ? size_names[size] : NULL;
}

framesize_t camera_size_from_name(const char *name)
{
    for (int i = 0; i <= max_size && i < FRAMESIZE_INVALID; i++) {
        if (size_names[i] && strcasecmp(size_names[i], name) == 0) {
            return (framesize_t)i;
        }
    }
    return FRAMESIZE_INVALID;
}

framesize_t camera_get_size(void)
{
    return esp_camera_sensor_get()->status.framesize;
}

static void drop_frames(int n)
{
    for (int i = 0; i < n; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
}

esp_err_t camera_set_size(framesize_t size)
{
    if (size < 0 || size > max_size) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s->status.framesize == size) {
        return ESP_OK;
    }
    if (s->set_framesize(s, size) != 0) {
        ESP_LOGE(TAG, "sensor rejected size %s", camera_size_name(size));
        return ESP_FAIL;
    }
    drop_frames(3);
    return ESP_OK;
}

static const char *sensor_name(uint16_t pid)
{
    switch (pid) {
    case OV2640_PID: return "OV2640";
    case OV3660_PID: return "OV3660";
    case OV5640_PID: return "OV5640";
    case OV7725_PID: return "OV7725";
    case GC2145_PID: return "GC2145";
    default: return "unknown";
    }
}

static esp_err_t init_at(framesize_t size)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = size,
        .jpeg_quality = 12,
        // The driver's default guess (width * height / 5) is too tight for
        // detailed QXGA scenes, which then fail with "NO-EOI".
        .jpeg_buffer_size = 1024 * 1024,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&config);
}

esp_err_t camera_start(void)
{
    // Buffers are allocated for the size given at init, so start at the
    // largest size we use, then clamp to this sensor's maximum.
    esp_err_t err = init_at(CAMERA_MAX_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s - check board_pins.h", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    max_size = info ? info->max_size : CAMERA_DEFAULT_SIZE;
    if (max_size > CAMERA_MAX_SIZE) {
        max_size = CAMERA_MAX_SIZE;
    }
    ESP_LOGI(TAG, "sensor %s (PID 0x%04x), max size %s", sensor_name(s->id.PID), s->id.PID,
             camera_size_name(max_size));

    framesize_t start_size = CAMERA_DEFAULT_SIZE <= max_size ? CAMERA_DEFAULT_SIZE : max_size;
    if (s->set_framesize(s, start_size) != 0) {
        return ESP_FAIL;
    }
    drop_frames(10);  // let auto exposure / white balance settle
    return ESP_OK;
}
