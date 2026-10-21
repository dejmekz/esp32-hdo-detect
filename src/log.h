#pragma once

#include <Arduino.h>

#include "config.h"

// Log s časem od startu v ms, např. "[   12345] WiFi: connected".
#define LOG(fmt, ...)                                                   \
    do {                                                                \
        if (SERIAL_LOGGING) {                                           \
            Serial.printf("[%8lu] " fmt "\n", millis(), ##__VA_ARGS__); \
        }                                                               \
    } while (0)
