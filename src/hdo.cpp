#include "hdo.h"

#include <esp_timer.h>

#include <debounce.h>

#include "config.h"
#include "log.h"

// Odfiltrovaný stav = jediný zdroj pravdy pro MQTT, OLED i LED.
static Debouncer debouncer(HDO_DEBOUNCE_MS);
// Čas poslední změny tarifu (µs od startu). esp_timer je 64bitový,
// millis() by přetekl po 49 dnech.
static int64_t lastChangeUs = 0;

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

    const int64_t now = esp_timer_get_time();
    LOG("HDO: tariff %s (previous lasted %lu s)", hdoTariffText(),
        static_cast<unsigned long>((now - lastChangeUs) / 1000000));
    lastChangeUs = now;
    return true;
}

uint32_t hdoTariffSeconds()
{
    return static_cast<uint32_t>((esp_timer_get_time() - lastChangeUs) / 1000000);
}

bool hdoLowTariff()
{
    return debouncer.state();
}

const char* hdoTariffText()
{
    return hdoLowTariff() ? "NT" : "VT";
}
