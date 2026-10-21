#include "hdo.h"

#include <debounce.h>

#include "config.h"
#include "log.h"

// Odfiltrovaný stav = jediný zdroj pravdy pro MQTT, OLED i LED.
static Debouncer debouncer(HDO_DEBOUNCE_MS);

static bool readHdo()
{
    return digitalRead(HDO_PIN) == HDO_ACTIVE_LEVEL;
}

void hdoSetup()
{
    // Interní pull-up jako záloha k externímu rezistoru.
    pinMode(HDO_PIN, INPUT_PULLUP);
    debouncer.reset(readHdo(), millis());
}

bool hdoUpdate()
{
    if (!debouncer.update(readHdo(), millis())) return false;

    LOG("HDO: tariff %s", hdoTariffText());
    return true;
}

bool hdoLowTariff()
{
    return debouncer.state();
}

const char* hdoTariffText()
{
    return hdoLowTariff() ? "NT" : "VT";
}
