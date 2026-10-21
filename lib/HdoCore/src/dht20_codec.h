#pragma once

#include <stddef.h>
#include <stdint.h>

// Dekódování dat senzoru DHT20 (AHT20).
// Bez Arduino API - testy v test/test_core.

// Stavový bajt: bit 7 = převod probíhá.
inline bool dht20Busy(uint8_t status)
{
    return (status & 0x80) != 0;
}

// Stavový bajt: bity 3 a 4 = kalibrace načtená. Jinak je potřeba
// inicializace registrů 0x1B, 0x1C a 0x1E.
inline bool dht20Calibrated(uint8_t status)
{
    return (status & 0x18) == 0x18;
}

// CRC-8: polynom 0x31, počáteční hodnota 0xFF.
inline uint8_t dht20Crc(const uint8_t* data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
    }
    return crc;
}

struct Dht20Reading {
    float temperature;  // °C
    float humidity;     // %
};

// data = 7 bajtů odpovědi na měření (stav, 5 B hodnot, CRC).
// false = chybné CRC, out se nemění.
inline bool dht20Decode(const uint8_t* data, Dht20Reading& out)
{
    if (dht20Crc(data, 6) != data[6]) return false;

    // 20 bitů vlhkost, 20 bitů teplota (rozsah -50..150 °C).
    const uint32_t rawHumidity = (static_cast<uint32_t>(data[1]) << 12) |
                                 (static_cast<uint32_t>(data[2]) << 4) |
                                 (data[3] >> 4);
    const uint32_t rawTemperature =
        (static_cast<uint32_t>(data[3] & 0x0F) << 16) |
        (static_cast<uint32_t>(data[4]) << 8) | data[5];
    out.humidity = rawHumidity * 100.0f / 1048576.0f;
    out.temperature = rawTemperature * 200.0f / 1048576.0f - 50.0f;
    return true;
}
