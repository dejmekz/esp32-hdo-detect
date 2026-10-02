#pragma once

// Šablona přihlašovacích údajů.
// Zkopíruj jako src/secrets.h a vyplň vlastní hodnoty.

constexpr char WIFI_SSID[] = "your-ssid";
constexpr char WIFI_PASSWORD[] = "your-wifi-password";

// Prázdné MQTT_USER = připojení bez autentizace.
constexpr char MQTT_USER[] = "";
constexpr char MQTT_PASSWORD[] = "";

// Heslo pro OTA aktualizace (stejné heslo patří do ota.secret.ini).
// Prázdné = OTA bez hesla (nedoporučeno).
constexpr char OTA_PASSWORD[] = "your-ota-password";
