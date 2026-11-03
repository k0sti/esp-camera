#pragma once

// Camera wiring for the common ESP32-S3 N16R8 "CAM" boards (same as
// Espressif ESP32-S3-EYE and Freenove ESP32-S3-WROOM). -1 = not connected.
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  15
#define CAM_PIN_SIOD  4
#define CAM_PIN_SIOC  5

#define CAM_PIN_D7    16
#define CAM_PIN_D6    17
#define CAM_PIN_D5    18
#define CAM_PIN_D4    12
#define CAM_PIN_D3    10
#define CAM_PIN_D2    8
#define CAM_PIN_D1    9
#define CAM_PIN_D0    11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF  7
#define CAM_PIN_PCLK  13

// Capture triggers. The button is the on-board BOOT button (active low).
// The sensor input fires on a rising edge; -1 disables it.
#define TRIGGER_PIN_BUTTON 0
#define TRIGGER_PIN_SENSOR 14
