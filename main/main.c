// Connects to Wi-Fi, then captures a JPEG and POSTs it to the server whenever
// a trigger fires: the BOOT button, the sensor input, or HTTP GET /capture.

#include "esp_log.h"

#include "camera.h"
#include "triggers.h"
#include "uploader.h"
#include "wifi.h"

static const char *TAG = "main";

void app_main(void)
{
    static app_config_t config;
    if (config_load(&config) != ESP_OK) {
        return;
    }
    if (camera_start() != ESP_OK) {
        return;
    }

    ESP_ERROR_CHECK(wifi_connect(&config));
    uploader_init(config.server_url);
    ESP_ERROR_CHECK(triggers_start());
    ESP_LOGI(TAG, "ready");
}
