#pragma once

// Verze z git describe (scripts/version.py) - v hdo/state, v HA
// (sw_version, i s časem sestavení) a v logu je vidět, zda OTA prošla.
#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif

constexpr char FIRMWARE_BUILD[] = FW_VERSION " " __DATE__ " " __TIME__;
