// Compile the existing Arduino lifecycle without maintaining a second copy.
#include "esp_arduino_version.h"
#if ESP_ARDUINO_VERSION != ESP_ARDUINO_VERSION_VAL(3, 3, 12)
#error "singlemic integration requires Arduino ESP32 3.3.12"
#endif
#include "../singlemic.ino"
