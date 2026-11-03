#pragma once

#include "esp_camera.h"
#include "esp_err.h"

#define CAMERA_DEFAULT_SIZE FRAMESIZE_UXGA

// Largest size offered. The 2560-wide OV5640 sizes (QHD, WQXGA, QSXGA) produce
// corrupt frames ("NO-EOI") with this setup, so they are left out.
#define CAMERA_MAX_SIZE FRAMESIZE_QXGA

// Initialise the camera and let auto exposure settle. Frame buffers are sized
// for the sensor's largest resolution so camera_set_size() can pick any size.
esp_err_t camera_start(void);

// Largest frame size the connected sensor supports.
framesize_t camera_max_size(void);

// Name of a frame size ("UXGA", "96X96", ...), or NULL if invalid.
const char *camera_size_name(framesize_t size);

// Frame size for a name (case-insensitive), or FRAMESIZE_INVALID if the name
// is unknown or larger than the sensor supports.
framesize_t camera_size_from_name(const char *name);

framesize_t camera_get_size(void);

// Switch resolution and drop frames captured at the old size.
esp_err_t camera_set_size(framesize_t size);
