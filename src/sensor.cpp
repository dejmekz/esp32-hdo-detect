#include "sensor.h"

#include <Wire.h>
#include <math.h>

#include <dht20_codec.h>

#include "config.h"
#include "log.h"

// Měření běží na pozadí (spuštění -> po 80 ms čtení), smyčka na převod
// nečeká.
static bool ready = false;
static bool measuring = false;
static bool hasValue = false;
// "not found" se loguje jen jednou, ne při každém dalším hledání.
static bool missingLogged = false;
static uint8_t failures = 0;
static Dht20Reading reading = {NAN, NAN};
static unsigned long measureStart = 0;
static unsigned long lastValue = 0;
static unsigned long lastProbe = 0;

static bool dht20Write(const uint8_t* data, size_t len)
{
    Wire.beginTransmission(DHT20_I2C_ADDRESS);
    Wire.write(data, len);
    return Wire.endTransmission() == 0;
}

// Stavový bajt, -1 = senzor neodpovídá.
static int dht20ReadStatus()
{
    if (Wire.requestFrom(DHT20_I2C_ADDRESS, static_cast<uint8_t>(1)) != 1) {
        return -1;
    }
    return Wire.read();
}

// Inicializace registru podle vzorového kódu výrobce (Aosong):
// přečíst registr a zapsat jeho hodnotu zpět příkazem 0xB0 | reg.
static void dht20ResetRegister(uint8_t reg)
{
    const uint8_t read[] = {reg, 0x00, 0x00};
    dht20Write(read, sizeof(read));
    delay(5);

    uint8_t value[3] = {0, 0, 0};
    const uint8_t count =
        Wire.requestFrom(DHT20_I2C_ADDRESS, static_cast<uint8_t>(sizeof(value)));
    for (uint8_t i = 0; i < count && i < sizeof(value); i++) {
        value[i] = Wire.read();
    }
    delay(10);

    const uint8_t write[] = {static_cast<uint8_t>(0xB0 | reg), value[1], value[2]};
    dht20Write(write, sizeof(write));
    delay(5);
}

// Zjistí, zda DHT20 odpovídá; při úspěchu ho připraví k měření.
static bool probe()
{
    const int status = dht20ReadStatus();
    if (status < 0) {
        if (!missingLogged) {
            LOG("DHT20: not found at 0x%02X", DHT20_I2C_ADDRESS);
            missingLogged = true;
        }
        return false;
    }

    if (!dht20Calibrated(status)) {
        dht20ResetRegister(0x1B);
        dht20ResetRegister(0x1C);
        dht20ResetRegister(0x1E);
        delay(10);
        const int newStatus = dht20ReadStatus();
        LOG("DHT20: status 0x%02X, initialized, now 0x%02X", status, newStatus);
    }

    ready = true;
    measuring = false;
    failures = 0;
    missingLogged = false;
    // První měření hned v příštím průchodu smyčkou.
    measureStart = millis() - SENSOR_READ_INTERVAL_MS;
    LOG("DHT20: found at 0x%02X", DHT20_I2C_ADDRESS);
    return true;
}

// Chyba měření. Po SENSOR_MAX_FAILURES v řadě se senzor považuje za
// odpojený a sensorUpdate() ho začne znovu hledat.
static void measurementFailed()
{
    if (++failures < SENSOR_MAX_FAILURES) return;

    LOG("DHT20: %u failures in a row, sensor lost", failures);
    ready = false;
    measuring = false;
    lastProbe = millis();
}

void sensorSetup()
{
    // Senzor potřebuje po zapnutí napájení 100 ms.
    while (millis() < DHT20_POWERUP_MS) {
        delay(10);
    }

    lastProbe = millis();
    probe();
}

// Spustí měření každých SENSOR_READ_INTERVAL_MS a výsledek přečte
// až po DHT20_MEASURE_MS, takže smyčku neblokuje.
bool sensorUpdate()
{
    const unsigned long now = millis();

    if (!ready) {
        if (now - lastProbe < SENSOR_PROBE_INTERVAL_MS) return false;
        lastProbe = now;
        return probe();
    }

    if (!measuring) {
        if (now - measureStart < SENSOR_READ_INTERVAL_MS) return false;
        measureStart = now;
        static const uint8_t trigger[] = {0xAC, 0x33, 0x00};
        if (dht20Write(trigger, sizeof(trigger))) {
            measuring = true;
        } else {
            LOG("DHT20: trigger failed");
            measurementFailed();
        }
        return false;
    }

    if (now - measureStart < DHT20_MEASURE_MS) return false;

    uint8_t data[7];
    const uint8_t count =
        Wire.requestFrom(DHT20_I2C_ADDRESS, static_cast<uint8_t>(sizeof(data)));
    for (uint8_t i = 0; i < count && i < sizeof(data); i++) {
        data[i] = Wire.read();
    }

    if (count == sizeof(data) && dht20Busy(data[0])) {
        // Převod ještě běží - zkusit v dalším průchodu, nejvýš do timeoutu.
        if (now - measureStart < DHT20_MEASURE_TIMEOUT_MS) return false;
        LOG("DHT20: measurement timeout");
        measuring = false;
        measurementFailed();
        return false;
    }
    measuring = false;

    if (count != sizeof(data)) {
        LOG("DHT20: read failed (%u B)", count);
        measurementFailed();
        return false;
    }

    Dht20Reading value;
    if (!dht20Decode(data, value)) {
        LOG("DHT20: CRC error");
        measurementFailed();
        return false;
    }
    failures = 0;
    reading = value;

    if (!hasValue) {
        LOG("DHT20: %.1f °C, %.1f %%", reading.temperature, reading.humidity);
    }
    hasValue = true;
    lastValue = now;
    return false;
}

bool sensorPresent()
{
    return ready;
}

bool sensorFresh()
{
    return hasValue && millis() - lastValue < SENSOR_STALE_MS;
}

float sensorTemperature()
{
    return reading.temperature;
}

float sensorHumidity()
{
    return reading.humidity;
}
