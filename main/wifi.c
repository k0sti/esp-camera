#include "wifi.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

extern const char wifi_txt_start[] asm("_binary_wifi_txt_start");
extern const char wifi_txt_end[] asm("_binary_wifi_txt_end");

#define CONNECTED_BIT BIT0
static EventGroupHandle_t wifi_events;

static void copy_trimmed(char *dst, size_t dst_size, const char *src, size_t len)
{
    while (len > 0 && isspace((unsigned char)*src)) {
        src++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)src[len - 1])) {
        len--;
    }
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

esp_err_t config_load(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->server_url, CONFIG_CAM_SERVER_URL, sizeof(cfg->server_url));

    const char *line = wifi_txt_start;
    while (line < wifi_txt_end && *line) {
        const char *eol = memchr(line, '\n', wifi_txt_end - line);
        if (!eol) {
            eol = wifi_txt_end;
        }
        const char *sep = line;
        while (sep < eol && *sep != ':' && *sep != '=') {
            sep++;
        }
        if (sep < eol) {
            char key[16];
            copy_trimmed(key, sizeof(key), line, sep - line);
            const char *value = sep + 1;
            size_t value_len = eol - value;
            if (strcasecmp(key, "ssid") == 0) {
                copy_trimmed(cfg->ssid, sizeof(cfg->ssid), value, value_len);
            } else if (strcasecmp(key, "passwd") == 0 || strcasecmp(key, "password") == 0) {
                copy_trimmed(cfg->password, sizeof(cfg->password), value, value_len);
            } else if (strcasecmp(key, "server") == 0) {
                copy_trimmed(cfg->server_url, sizeof(cfg->server_url), value, value_len);
            }
        }
        line = eol + 1;
    }

    if (cfg->ssid[0] == '\0') {
        ESP_LOGE(TAG, "wifi.txt has no SSID line");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        xEventGroupClearBits(wifi_events, CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected (reason %d), retrying", event->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, CONNECTED_BIT);
    }
}

esp_err_t wifi_connect(const app_config_t *cfg)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, "esp-camera");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, cfg->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->password, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep adds 100+ ms to every round trip, which throttles uploads.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "connecting to \"%s\"", cfg->ssid);
    xEventGroupWaitBits(wifi_events, CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return wifi_events && (xEventGroupGetBits(wifi_events) & CONNECTED_BIT);
}
