#include "ota.h"

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <string.h>

#include "config.h"
#include "display.h"
#include "led.h"
#include "log.h"
#include "network.h"

static bool ready = false;
// onError přijde i bez onStart (např. špatné heslo); výkon Wi-Fi se
// pak nevrací, protože se neměnil.
static bool updating = false;
static int lastPercent = -1;
static wifi_power_t normalTxPower = WIFI_POWER_19_5dBm;

static const char* otaErrorText(ota_error_t error)
{
    switch (error) {
        case OTA_AUTH_ERROR:    return "auth failed";
        case OTA_BEGIN_ERROR:   return "begin failed";
        case OTA_CONNECT_ERROR: return "connect failed";
        case OTA_RECEIVE_ERROR: return "receive failed";
        case OTA_END_ERROR:     return "end failed";
        default:                return "unknown";
    }
}

void otaSetup()
{
    ArduinoOTA.setHostname(DEVICE_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) {
        ArduinoOTA.setPassword(OTA_PASSWORD);
    }

    ArduinoOTA.onStart([]() {
        LOG("OTA: start (%s)",
            ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
        updating = true;
        lastPercent = -1;
        // Nižší výkon = nižší proudové špičky => bez brownout resetu.
        normalTxPower = WiFi.getTxPower();
        WiFi.setTxPower(OTA_WIFI_TX_POWER);
        mqttGoOffline();
        displayMessage("OTA UPDATE", "Starting...");
        ledSet(LedColor::Blue);
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        // Celý upload běží uvnitř ArduinoOTA.handle(), loop() mezitím
        // neproběhne - watchdog se proto resetuje po každém bloku dat.
        esp_task_wdt_reset();

        const int percent = total > 0 ? progress * 100 / total : 0;
        if (percent == lastPercent) return;
        lastPercent = percent;

        if (percent % 10 == 0) LOG("OTA: %d %%", percent);
        // loop() neběží, LED se přepíná přímo zde: modře problikává.
        ledSet(percent % 2 == 0 ? LedColor::Blue : LedColor::Off);

        char line[24];
        snprintf(line, sizeof(line), "Progress: %d %%", percent);
        displayMessage("OTA UPDATE", line);
    });

    ArduinoOTA.onEnd([]() {
        LOG("OTA: done, restarting");
        displayMessage("OTA UPDATE", "Done, restarting...");
        ledSet(LedColor::Blue);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        LOG("OTA: error %u (%s)", error, otaErrorText(error));
        if (updating) {
            WiFi.setTxPower(normalTxPower);
            updating = false;
        }
        displayHoldMessage("OTA ERROR", otaErrorText(error));
        ledHoldError();
    });

    ArduinoOTA.begin();
    ready = true;
    LOG("OTA: ready at %s.local%s", DEVICE_HOSTNAME,
        strlen(OTA_PASSWORD) > 0 ? "" : " (NO PASSWORD)");
}

bool otaReady()
{
    return ready;
}

void otaHandle()
{
    if (ready) ArduinoOTA.handle();
}
