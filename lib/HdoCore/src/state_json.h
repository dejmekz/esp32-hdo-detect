#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Obsah zprávy hdo/state. Bez Arduino API - testy v test/test_core.
struct StateInfo {
    bool lowTariff;
    int rssi;
    uint32_t uptimeS;
    const char* ip;
    const char* resetReason;
    const char* firmware;
    // Volná paměť heapu teď a nejnižší od startu (B) - únik paměti
    // se projeví postupným poklesem minFreeHeap.
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    // Jak dlouho (s) trvá aktuální tarif - od poslední změny, nebo od
    // startu zařízení, pokud se od něj tarif nezměnil.
    uint32_t tariffS;
    // false = teplota a vlhkost se publikují jako null.
    bool climateValid;
    float temperature;
    float humidity;
};

// JSON pro hdo/state. Klíč "hdo" zůstává kvůli kompatibilitě. Teplota
// a vlhkost bez aktuálního měření = null (klíč zůstává, aby JSONPath
// v openHAB nehlásil chybějící cestu).
// false = buffer je příliš malý, obsah nelze použít.
inline bool formatStateJson(char* buf, size_t size, const StateInfo& s)
{
    char climate[48] = ",\"temperature\":null,\"humidity\":null";
    if (s.climateValid) {
        snprintf(climate, sizeof(climate),
                 ",\"temperature\":%.1f,\"humidity\":%.1f",
                 s.temperature, s.humidity);
    }

    const int len = snprintf(
        buf, size,
        "{\"hdo\":%s,\"rssi\":%d,\"uptime\":%lu,\"ip\":\"%s\","
        "\"reset\":\"%s\",\"fw\":\"%s\",\"heap\":%lu,\"heap_min\":%lu,"
        "\"tariff_s\":%lu%s}",
        s.lowTariff ? "true" : "false",
        s.rssi,
        static_cast<unsigned long>(s.uptimeS),
        s.ip,
        s.resetReason,
        s.firmware,
        static_cast<unsigned long>(s.freeHeap),
        static_cast<unsigned long>(s.minFreeHeap),
        static_cast<unsigned long>(s.tariffS),
        climate);
    return len >= 0 && static_cast<size_t>(len) < size;
}
