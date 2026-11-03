#pragma once

#include "esp_err.h"

// Start the GPIO triggers (button, sensor) and the HTTP trigger
// (GET /capture on port 80). Each one calls capture_and_upload().
esp_err_t triggers_start(void);
