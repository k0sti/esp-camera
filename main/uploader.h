#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_camera.h"
#include "esp_err.h"

typedef struct {
    int http_status;     // server response code, 0 if the request never completed
    size_t bytes;        // JPEG size
    uint16_t width;
    uint16_t height;
    uint32_t capture_ms; // waiting for a frame from the camera
    uint32_t upload_ms;  // HTTP POST, including connection setup
} upload_result_t;

typedef struct {
    int count;            // images attempted
    int ok;               // images captured (and uploaded, if requested)
    uint16_t width;
    uint16_t height;
    size_t bytes_total;
    size_t bytes_min;
    size_t bytes_max;
    uint32_t capture_ms;  // summed over all images
    uint32_t upload_ms;   // summed over all images
    uint32_t elapsed_ms;  // whole burst, excluding the resolution switch
} burst_result_t;

void uploader_init(const char *server_url);

// Capture one JPEG and POST it to the server. Safe to call from several
// tasks; captures are serialised. `trigger` names what caused the capture.
esp_err_t capture_and_upload(const char *trigger, upload_result_t *result);

// Capture `count` images at `size` back to back, uploading each one if
// `upload` is set (trigger "test-<SIZE>"). Restores the previous size after.
esp_err_t capture_burst(framesize_t size, int count, bool upload, burst_result_t *result);
