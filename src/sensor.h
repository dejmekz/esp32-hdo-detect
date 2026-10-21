#pragma once

// Senzor teploty a vlhkosti DHT20 na I2C (Wire musí být spuštěný).
// Volitelný: chybějící senzor se hledá každých SENSOR_PROBE_INTERVAL_MS.

void sensorSetup();

// Měření na pozadí, volat v každém průchodu smyčkou.
// true = senzor byl právě nalezen (připojen za běhu).
bool sensorUpdate();

bool sensorPresent();
// Platná a ne příliš stará hodnota (SENSOR_STALE_MS).
bool sensorFresh();
float sensorTemperature();
float sensorHumidity();
