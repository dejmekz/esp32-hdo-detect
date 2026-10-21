#pragma once

#include <led_logic.h>

// Stavová RGB LED (PWM přes LEDC).

void ledSetup();
void ledSet(LedColor color);

// Barva podle tarifu a připojení (viz ledStatusColor); po chybě OTA
// místo toho ERROR_HOLD_MS rychle bliká fialově.
void ledRefresh(bool lowTariff, bool wifiConnected, bool mqttConnected);

// Spustí fialové blikání po chybě OTA.
void ledHoldError();
