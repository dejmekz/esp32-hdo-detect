#include "restart.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>

// Libovolná hodnota, která se po zapnutí napájení v neinicializované
// paměti náhodně neobjeví.
static constexpr uint32_t COMMAND_RESTART_MAGIC = 0x52535443;  // "RSTC"

RTC_NOINIT_ATTR static uint32_t restartMarker;

void restartOnCommand()
{
    restartMarker = COMMAND_RESTART_MAGIC;
    ESP.restart();
}

bool restartedOnCommand()
{
    const bool onCommand = esp_reset_reason() == ESP_RST_SW &&
                           restartMarker == COMMAND_RESTART_MAGIC;
    restartMarker = 0;
    return onCommand;
}
