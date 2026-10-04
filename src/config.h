#pragma once

#include <Arduino.h>
#include <WiFi.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Chybí src/secrets.h - zkopíruj src/secrets.example.h na src/secrets.h a vyplň údaje."
#endif

// ============================================================
// Hardware - LaskaKit ESP32-C3, revize 2.1
// ============================================================

constexpr uint8_t HDO_PIN = 1;          // HDO přes PC817C
constexpr uint8_t OLED_SDA_PIN = 8;     // OLED I2C SDA
constexpr uint8_t OLED_SCL_PIN = 10;    // OLED I2C SCL
constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;
constexpr int8_t OLED_RESET_PIN = -1;

// Dvoubarevný OLED: řádky 0-15 žluté (hlavička), 16-63 modré.
// Font velikosti 1 má výšku 8 px.
constexpr int16_t OLED_HEADER_Y = 4;     // vystředěno ve žlutém pásu
constexpr int16_t OLED_BODY_Y = 18;      // první řádek v modré části
constexpr int16_t OLED_LINE_HEIGHT = 12; // 4 řádky: 18..61

// HDO active = LOW. Interní pull-up je zapnutý jako záloha,
// doporučen je i externí pull-up na 3.3 V (např. 10 kΩ).
constexpr uint8_t HDO_ACTIVE_LEVEL = LOW;

// ============================================================
// Timing
// ============================================================

constexpr unsigned long HDO_DEBOUNCE_MS = 50;
constexpr unsigned long REPORT_INTERVAL_MS = 60000;
// Jak dlouho nechat běžet automatický reconnect, než se Wi-Fi
// restartuje ručně (disconnect + begin).
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
constexpr unsigned long MDNS_QUERY_TIMEOUT_MS = 2000;
constexpr unsigned long OLED_REFRESH_INTERVAL_MS = 500;
// Watchdog hlavní smyčky: když loop() nedoběhne do této doby,
// zařízení se restartuje (důvod resetu "task watchdog"). Musí být
// delší než nejdelší blokující volání (mDNS 2 s, MQTT connect).
constexpr uint32_t WATCHDOG_TIMEOUT_S = 30;

// ============================================================
// Síť, OTA, logování
// ============================================================

// Hostname pro Wi-Fi, mDNS a OTA => OTA adresa "hdo-monitor.local".
constexpr char DEVICE_HOSTNAME[] = "hdo-monitor";

// Vysílací výkon Wi-Fi během OTA. Při plném výkonu (19.5 dBm) způsoboval
// souběžný zápis do flash a vysílání brownout reset desky.
constexpr wifi_power_t OTA_WIFI_TX_POWER = WIFI_POWER_11dBm;

// Logování na Serial (USB CDC). false = vypnuto.
constexpr bool SERIAL_LOGGING = true;
constexpr unsigned long SERIAL_BAUD = 115200;
// Jak dlouho po startu čekat na otevřený sériový monitor.
constexpr unsigned long SERIAL_WAIT_MS = 1500;
// Max. čekání zápisu na USB, když port nikdo nečte (musí být > 0).
constexpr uint32_t SERIAL_TX_TIMEOUT_MS = 10;

// ============================================================
// MQTT
// ============================================================

// Hostname končící na ".local" se překládá přes mDNS.
// Spolehlivější je pevná IP adresa, např. "192.168.1.10".
constexpr char MQTT_HOST[] = "raspi4server.local";
constexpr uint16_t MQTT_PORT = 1883;

// MQTT topics - zachována kompatibilita s původním projektem.
constexpr char MQTT_HDO_TOPIC[] = "hdo/status";
constexpr char MQTT_STATE_TOPIC[] = "hdo/state";
constexpr char MQTT_AVAILABILITY_TOPIC[] = "hdo/availability";
