#include "uploader.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "camera.h"
#include "wifi.h"

static const char *TAG = "upload";

static const char *upload_url;
static char device_id[13];
static SemaphoreHandle_t capture_lock;

static uint32_t ms_since(int64_t start_us)
{
    return (uint32_t)((esp_timer_get_time() - start_us) / 1000);
}

void uploader_init(const char *server_url)
{
    upload_url = server_url;
    capture_lock = xSemaphoreCreateMutex();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "device %s uploads to %s", device_id, upload_url);
}

static esp_err_t post_jpeg(const camera_fb_t *fb, const char *trigger, int *status)
{
    esp_http_client_config_t config = {
        .url = upload_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(client, "X-Device-Id", device_id);
    esp_http_client_set_header(client, "X-Trigger", trigger);

    *status = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, fb->len);
    int64_t t1 = esp_timer_get_time();
    int64_t t2 = t1;
    if (err == ESP_OK) {
        int written = esp_http_client_write(client, (const char *)fb->buf, fb->len);
        t2 = esp_timer_get_time();
        if (written != (int)fb->len || esp_http_client_fetch_headers(client) < 0) {
            err = ESP_FAIL;
        } else {
            *status = esp_http_client_get_status_code(client);
        }
    }
    int64_t t3 = esp_timer_get_time();
    ESP_LOGI(TAG, "%s: %u bytes: connect %lld ms, send %lld ms, response %lld ms", trigger,
             (unsigned)fb->len, (t1 - t0) / 1000, (t2 - t1) / 1000, (t3 - t2) / 1000);
    esp_http_client_cleanup(client);
    return err;
}

// Caller holds capture_lock.
static esp_err_t capture_one(const char *trigger, bool upload, upload_result_t *result)
{
    *result = (upload_result_t){0};

    int64_t start = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    result->capture_ms = ms_since(start);
    if (!fb) {
        ESP_LOGE(TAG, "%s: capture failed", trigger);
        return ESP_FAIL;
    }
    result->bytes = fb->len;
    result->width = fb->width;
    result->height = fb->height;

    esp_err_t err = ESP_OK;
    if (upload) {
        start = esp_timer_get_time();
        err = post_jpeg(fb, trigger, &result->http_status);
        result->upload_ms = ms_since(start);
    }
    esp_camera_fb_return(fb);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: POST failed: %s", trigger, esp_err_to_name(err));
        return err;
    }
    if (upload && (result->http_status < 200 || result->http_status >= 300)) {
        ESP_LOGE(TAG, "%s: server answered %d", trigger, result->http_status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t capture_and_upload(const char *trigger, upload_result_t *result)
{
    upload_result_t local;
    if (!result) {
        result = &local;
    }
    *result = (upload_result_t){0};

    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "%s: not connected to Wi-Fi", trigger);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(capture_lock, portMAX_DELAY);
    esp_err_t err = capture_one(trigger, true, result);
    xSemaphoreGive(capture_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: uploaded %u bytes", trigger, (unsigned)result->bytes);
    }
    return err;
}

esp_err_t capture_burst(framesize_t size, int count, bool upload, burst_result_t *result)
{
    *result = (burst_result_t){0};
    if (upload && !wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    char trigger[24];
    snprintf(trigger, sizeof(trigger), "test-%s", camera_size_name(size));

    xSemaphoreTake(capture_lock, portMAX_DELAY);
    framesize_t previous = camera_get_size();
    esp_err_t err = camera_set_size(size);
    if (err != ESP_OK) {
        xSemaphoreGive(capture_lock);
        return err;
    }

    int64_t start = esp_timer_get_time();
    for (int i = 0; i < count; i++) {
        upload_result_t r;
        result->count++;
        if (capture_one(trigger, upload, &r) != ESP_OK) {
            continue;
        }
        result->ok++;
        result->width = r.width;
        result->height = r.height;
        result->bytes_total += r.bytes;
        if (result->bytes_min == 0 || r.bytes < result->bytes_min) {
            result->bytes_min = r.bytes;
        }
        if (r.bytes > result->bytes_max) {
            result->bytes_max = r.bytes;
        }
        result->capture_ms += r.capture_ms;
        result->upload_ms += r.upload_ms;
    }
    result->elapsed_ms = ms_since(start);

    camera_set_size(previous);
    xSemaphoreGive(capture_lock);

    ESP_LOGI(TAG, "%s: %d/%d ok in %" PRIu32 " ms (capture %" PRIu32 " ms, upload %" PRIu32 " ms)",
             trigger, result->ok, result->count, result->elapsed_ms, result->capture_ms,
             result->upload_ms);
    return result->ok == result->count ? ESP_OK : ESP_FAIL;
}
