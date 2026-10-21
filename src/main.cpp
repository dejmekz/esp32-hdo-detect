#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "display.h"
#include "hdo.h"
#include "led.h"
#include "log.h"
#include "network.h"
#include "ota.h"
#include "sensor.h"
#include "version.h"

static const char* resetReasonText(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:  return "power on";
        case ESP_RST_EXT:      return "external pin";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        default:               return "unknown";
    }
}

static void setupSerial()
{
    if (!SERIAL_LOGGING) return;

    Serial.begin(SERIAL_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT
    // Když je USB v PC, ale port nikdo nečte, zápis po timeoutu vzdá
    // a log se zahazuje. Hodnota 0 NESMÍ být použita: HWCDC pak
    // podteče počítadlo pokusů a write() zablokuje smyčku natrvalo.
    Serial.setTxTimeoutMs(SERIAL_TX_TIMEOUT_MS);
    // Krátce počkat na sériový monitor, ať nezmizí úvodní výpis.
    while (!Serial && millis() < SERIAL_WAIT_MS) {
        delay(10);
    }
#endif
}

static void setupWatchdog()
{
    // Arduino už TWDT spouští (5 s, jen idle task); v IDF 4.4 volání
    // esp_task_wdt_init() změní timeout existujícího watchdogu.
    // panic = true => restart s důvodem "task watchdog".
    esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
    esp_task_wdt_add(nullptr);  // hlídat i loopTask (aktuální task)
    LOG("Watchdog: %lu s", static_cast<unsigned long>(WATCHDOG_TIMEOUT_S));
}

void setup()
{
    ledSetup();
    setupSerial();
    setupWatchdog();
    hdoSetup();

    const esp_reset_reason_t resetReason = esp_reset_reason();
    const char* resetReasonName = resetReasonText(resetReason);
    LOG("HDO monitor start, build %s, reset reason %d (%s)",
        FIRMWARE_BUILD, static_cast<int>(resetReason), resetReasonName);
    LOG("HDO: initial tariff %s", hdoTariffText());

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    sensorSetup();
    displaySetup();

    networkSetup(resetReasonName);
}

void loop()
{
    esp_task_wdt_reset();

    networkLoop();
    otaHandle();

    if (hdoUpdate()) {
        publishHdo();
        publishState();
    }
    // Senzor připojený za běhu: HA entity teploty a vlhkosti se při
    // připojení k brokeru nepublikovaly.
    if (sensorUpdate()) {
        publishDiscovery();
    }

    ledRefresh(hdoLowTariff(), wifiConnected(), mqttConnected());
    displayRefresh(mqttConnected());
    delay(1);
}
