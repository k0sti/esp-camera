#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Settings read from wifi.txt, which is embedded in the firmware at build time.
typedef struct {
    char ssid[33];
    char password[65];
    char server_url[128];
} app_config_t;

// Parse wifi.txt ("key:value" or "key=value" lines). Keys: SSID,
// passwd/password, and optionally server (defaults to CONFIG_CAM_SERVER_URL).
esp_err_t config_load(app_config_t *cfg);

// Connect to Wi-Fi and block until an IP address is assigned. Reconnects
// automatically if the connection drops later.
esp_err_t wifi_connect(const app_config_t *cfg);

bool wifi_is_connected(void);
