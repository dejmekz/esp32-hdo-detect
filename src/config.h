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

// RGB LED se společnou katodou (na GND): HIGH = svítí.
// Pro LED se společnou anodou (na 3.3 V) nastav LED_COMMON_ANODE = true.
constexpr uint8_t LED_BLUE_PIN = 3;
constexpr uint8_t LED_RED_PIN = 4;
constexpr uint8_t LED_GREEN_PIN = 5;
constexpr bool LED_COMMON_ANODE = false;
// Jas přes PWM 0..255, zvlášť pro každou barvu (zelená a modrá
// bývají při stejném proudu výrazně jasnější než červená).
constexpr uint8_t LED_RED_BRIGHTNESS = 40;
constexpr uint8_t LED_GREEN_BRIGHTNESS = 40;
constexpr uint8_t LED_BLUE_BRIGHTNESS = 40;
constexpr uint8_t LED_RED_CHANNEL = 0;   // LEDC kanál
constexpr uint8_t LED_GREEN_CHANNEL = 1;
constexpr uint8_t LED_BLUE_CHANNEL = 2;
constexpr uint32_t LED_PWM_FREQ_HZ = 5000;
constexpr uint8_t LED_PWM_RESOLUTION = 8;

// Sběrnice I2C (μŠup konektor) - sdílí ji senzor DHT20 a OLED.
constexpr uint8_t I2C_SDA_PIN = 8;
constexpr uint8_t I2C_SCL_PIN = 10;

// Senzor teploty a vlhkosti Grove DHT20 (AHT20) - volitelný,
// při startu se detekuje; když chybí, hodnoty se nepublikují.
constexpr uint8_t DHT20_I2C_ADDRESS = 0x38;

// OLED displej - volitelný, zapíná se build flagem HDO_OLED=1
// v platformio.ini.
#ifndef HDO_OLED
#define HDO_OLED 0
#endif
constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;
constexpr int8_t OLED_RESET_PIN = -1;

// Dvoubarevný OLED: řádky 0-15 žluté (hlavička), 16-63 modré.
// Font velikosti 1 má výšku 8 px.
constexpr int16_t OLED_HEADER_Y = 4;     // vystředěno ve žlutém pásu
constexpr int16_t OLED_BODY_Y = 18;      // první řádek v modré části
constexpr int16_t OLED_LINE_HEIGHT = 12; // 4 řádky: 18..61

// Ochrana proti vypálení OLED (statický text 24/7):
// - nižší jas: 0..255, knihovna standardně nastaví 0xCF,
// - posun obsahu o pár pixelů každých OLED_SHIFT_INTERVAL_MS.
//   Posun Y je max. ±2 px, aby hlavička zůstala ve žlutém pásu
//   a poslední řádek na displeji.
constexpr uint8_t OLED_CONTRAST = 0x20;
constexpr unsigned long OLED_SHIFT_INTERVAL_MS = 5UL * 60 * 1000;

// HDO active = LOW. Interní pull-up je zapnutý jako záloha,
// doporučen je i externí pull-up na 3.3 V (např. 10 kΩ).
constexpr uint8_t HDO_ACTIVE_LEVEL = LOW;

// ============================================================
// Timing
// ============================================================

constexpr unsigned long HDO_DEBOUNCE_MS = 50;
constexpr unsigned long REPORT_INTERVAL_MS = 60000;
// DHT20: interval měření, doba převodu (datasheet 80 ms) a max. čekání
// na dokončení. Po zapnutí napájení potřebuje senzor 100 ms.
constexpr unsigned long SENSOR_READ_INTERVAL_MS = 10000;
constexpr unsigned long DHT20_MEASURE_MS = 80;
constexpr unsigned long DHT20_MEASURE_TIMEOUT_MS = 500;
constexpr unsigned long DHT20_POWERUP_MS = 100;
// Starší hodnota se nepublikuje (senzor přestal odpovídat).
constexpr unsigned long SENSOR_STALE_MS = 3 * SENSOR_READ_INTERVAL_MS;
// Jak dlouho nechat běžet automatický reconnect, než se Wi-Fi
// restartuje ručně (disconnect + begin).
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
// MQTT reconnect s exponenciálním odstupem: 5 s, 10 s, 20 s ... max 60 s.
// Neúspěšný pokus blokuje smyčku (mDNS + TCP connect), proto se při
// nedostupném brokeru nesmí opakovat příliš často.
constexpr unsigned long MQTT_RETRY_MIN_MS = 5000;
constexpr unsigned long MQTT_RETRY_MAX_MS = 60000;
// Timeout TCP spojení s brokerem (výchozí ve WiFiClient jsou 3 s).
// Platí i pro zápis do socketu.
constexpr uint32_t MQTT_CONNECT_TIMEOUT_S = 2;
// Po kolika neúspěšných pokusech v řadě znovu přeložit .local adresu.
constexpr uint8_t MQTT_RESOLVE_AFTER_FAILURES = 3;
constexpr unsigned long MDNS_QUERY_TIMEOUT_MS = 2000;
constexpr unsigned long OLED_REFRESH_INTERVAL_MS = 500;
// Jak dlouho nechat na OLED / LED chybovou hlášku (OTA ERROR).
constexpr unsigned long OLED_MESSAGE_HOLD_MS = 10000;
// Blikání LED: rychle = bez Wi-Fi, pomalu = Wi-Fi OK, ale bez MQTT.
// Hodnota = délka jedné fáze (svítí / nesvítí).
constexpr unsigned long LED_BLINK_FAST_MS = 150;
constexpr unsigned long LED_BLINK_SLOW_MS = 600;
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

// Buffer PubSubClient (výchozí 256 B) - discovery zprávy jsou delší.
constexpr uint16_t MQTT_BUFFER_SIZE = 768;

// Volitelné Home Assistant MQTT discovery: po připojení k brokeru se
// publikují (retained) konfigurace entit, HA zařízení přidá samo.
// Pro openHAB (ruční Thing, viz README) nech vypnuté.
constexpr bool HA_DISCOVERY = false;
constexpr char HA_DISCOVERY_PREFIX[] = "homeassistant";
