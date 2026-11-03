idf_build_get_property(project_dir PROJECT_DIR)
set(wifi_txt "${project_dir}/wifi.txt")
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT EXISTS "${wifi_txt}")
    message(FATAL_ERROR "wifi.txt not found. Create it with lines 'SSID:<name>' and 'passwd:<password>'.")
endif()

idf_component_register(SRCS "main.c" "camera.c" "wifi.c" "uploader.c" "triggers.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_psram esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp_http_server esp_driver_gpio esp_timer
                       EMBED_TXTFILES "${wifi_txt}")
