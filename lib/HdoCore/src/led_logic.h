#pragma once

#include <stdint.h>

// Logika stavové RGB LED. Bez Arduino API - testy v test/test_core.

enum class LedColor : uint8_t { Off, Red, Green, Blue, Magenta };

struct LedChannels {
    bool red;
    bool green;
    bool blue;
};

inline LedChannels ledChannels(LedColor color)
{
    return {
        color == LedColor::Red || color == LedColor::Magenta,
        color == LedColor::Green,
        color == LedColor::Blue || color == LedColor::Magenta,
    };
}

// Blikání: první fáze periody svítí, druhá ne.
inline LedColor ledBlink(LedColor color, uint32_t now, uint32_t phaseMs)
{
    return (now / phaseMs) % 2 == 0 ? color : LedColor::Off;
}

// NT = zelená, VT = červená. Svítí trvale, když je Wi-Fi i MQTT
// připojené; jinak bliká (rychle bez Wi-Fi, pomalu bez MQTT).
inline LedColor ledStatusColor(bool lowTariff, bool wifiConnected,
                               bool mqttConnected, uint32_t now,
                               uint32_t fastMs, uint32_t slowMs)
{
    const LedColor tariff = lowTariff ? LedColor::Green : LedColor::Red;
    if (!wifiConnected) return ledBlink(tariff, now, fastMs);
    if (!mqttConnected) return ledBlink(tariff, now, slowMs);
    return tariff;
}
