#include "triggers.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "camera.h"
#include "uploader.h"

static const char *TAG = "trigger";

// Ignore repeat edges from the same pin within this window (button bounce,
// a sensor holding its line active).
#define DEBOUNCE_US (1000 * 1000)

static QueueHandle_t gpio_events;

static void IRAM_ATTR on_gpio_edge(void *arg)
{
    uint32_t pin = (uint32_t)arg;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(gpio_events, &pin, &woken);
    portYIELD_FROM_ISR(woken);
}

static void gpio_trigger_task(void *arg)
{
    int64_t last_us[GPIO_NUM_MAX] = {0};
    uint32_t pin;
    while (true) {
        xQueueReceive(gpio_events, &pin, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        if (last_us[pin] != 0 && now - last_us[pin] < DEBOUNCE_US) {
            continue;
        }
        last_us[pin] = now;
        capture_and_upload(pin == TRIGGER_PIN_BUTTON ? "button" : "sensor", NULL);
        xQueueReset(gpio_events);  // drop edges that arrived during the upload
    }
}

static esp_err_t add_gpio_trigger(int pin, bool active_low)
{
    if (pin < 0) {
        return ESP_OK;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = active_low ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(pin, on_gpio_edge, (void *)(uint32_t)pin);
    }
    return err;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    upload_result_t result;
    esp_err_t err = capture_and_upload("http", &result);

    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":%s,\"bytes\":%u,\"server_status\":%d}\n",
             err == ESP_OK ? "true" : "false", (unsigned)result.bytes, result.http_status);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "502 Bad Gateway");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// GET /sizes: frame sizes this sensor supports.
static esp_err_t sizes_handler(httpd_req_t *req)
{
    char item[80];
    httpd_resp_set_type(req, "application/json");
    snprintf(item, sizeof(item), "{\"current\":\"%s\",\"sizes\":[",
             camera_size_name(camera_get_size()));
    httpd_resp_sendstr_chunk(req, item);
    for (framesize_t s = 0; s <= camera_max_size(); s++) {
        snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"width\":%u,\"height\":%u}",
                 s ? "," : "", camera_size_name(s), resolution[s].width, resolution[s].height);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}\n");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// GET /burst?size=UXGA&count=5&upload=1: capture images back to back and
// report timings. upload=0 measures the camera alone.
static esp_err_t burst_handler(httpd_req_t *req)
{
    char query[64] = "", value[16];
    httpd_req_get_url_query_str(req, query, sizeof(query));

    framesize_t size = camera_get_size();
    if (httpd_query_key_value(query, "size", value, sizeof(value)) == ESP_OK) {
        size = camera_size_from_name(value);
        if (size == FRAMESIZE_INVALID) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown or unsupported size");
        }
    }
    int count = 5;
    if (httpd_query_key_value(query, "count", value, sizeof(value)) == ESP_OK) {
        count = atoi(value);
    }
    if (count < 1 || count > 100) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "count must be 1-100");
    }
    bool upload = true;
    if (httpd_query_key_value(query, "upload", value, sizeof(value)) == ESP_OK) {
        upload = atoi(value) != 0;
    }

    burst_result_t r;
    esp_err_t err = capture_burst(size, count, upload, &r);

    char body[320];
    snprintf(body, sizeof(body),
             "{\"ok\":%s,\"size\":\"%s\",\"width\":%u,\"height\":%u,\"upload\":%s,"
             "\"count\":%d,\"succeeded\":%d,\"bytes_total\":%u,\"bytes_min\":%u,\"bytes_max\":%u,"
             "\"capture_ms\":%" PRIu32 ",\"upload_ms\":%" PRIu32 ",\"elapsed_ms\":%" PRIu32 "}\n",
             err == ESP_OK ? "true" : "false", camera_size_name(size), r.width, r.height,
             upload ? "true" : "false", r.count, r.ok, (unsigned)r.bytes_total,
             (unsigned)r.bytes_min, (unsigned)r.bytes_max, r.capture_ms, r.upload_ms,
             r.elapsed_ms);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "502 Bad Gateway");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t start_http_trigger(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;  // the handlers run the HTTP client too
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/capture", .method = HTTP_GET, .handler = capture_handler},
        {.uri = "/sizes", .method = HTTP_GET, .handler = sizes_handler},
        {.uri = "/burst", .method = HTTP_GET, .handler = burst_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]) && err == ESP_OK; i++) {
        err = httpd_register_uri_handler(server, &uris[i]);
    }
    return err;
}

esp_err_t triggers_start(void)
{
    gpio_events = xQueueCreate(8, sizeof(uint32_t));
    xTaskCreate(gpio_trigger_task, "gpio_trigger", 8192, NULL, 5, NULL);

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(add_gpio_trigger(TRIGGER_PIN_BUTTON, true));
    ESP_ERROR_CHECK(add_gpio_trigger(TRIGGER_PIN_SENSOR, false));

    esp_err_t err = start_http_trigger();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "button GPIO%d, sensor GPIO%d, HTTP GET /capture /burst /sizes",
                 TRIGGER_PIN_BUTTON, TRIGGER_PIN_SENSOR);
    }
    return err;
}
