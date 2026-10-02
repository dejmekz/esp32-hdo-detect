#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

// DS18B20 zatím není implementován. MQTT bude hlásit unknown/null
// a OLED zobrazí --.- C.

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Adafruit_SSD1306 display(128, 64, &Wire, OLED_RESET_PIN);

// hdoState = odfiltrovaný (debounced) stav; jediný zdroj pravdy
// pro MQTT i OLED.
bool hdoState = false;
bool hdoRawState = false;
bool oledReady = false;
bool mdnsStarted = false;
bool mqttServerResolved = false;

char mqttClientId[24];

unsigned long hdoRawChangeTime = 0;
unsigned long lastReport = 0;
unsigned long wifiDisconnectedSince = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastOledRefresh = 0;

bool readHdo()
{
    return digitalRead(HDO_PIN) == HDO_ACTIVE_LEVEL;
}

void publishAvailability(const char* state)
{
    if (mqttClient.connected()) {
        mqttClient.publish(MQTT_AVAILABILITY_TOPIC, state, true);
    }
}

void publishHdo()
{
    if (!mqttClient.connected()) return;

    mqttClient.publish(MQTT_HDO_TOPIC, hdoState ? "ON" : "OFF", true);
}

void publishTemperature()
{
    if (mqttClient.connected()) {
        mqttClient.publish(MQTT_TEMPERATURE_TOPIC, "unknown", true);
    }
}

void publishState()
{
    if (!mqttClient.connected()) return;

    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"hdo\":%s,\"temperature\":null}",
             hdoState ? "true" : "false");
    mqttClient.publish(MQTT_STATE_TOPIC, payload, true);
}

void publishAll()
{
    publishHdo();
    publishTemperature();
    publishState();
}

void startWiFi()
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(mqttClientId);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Běžné výpadky řeší automatický reconnect knihovny. Ruční restart
// Wi-Fi jen tehdy, když se nepřipojí ani po WIFI_RETRY_INTERVAL_MS.
void maintainWiFi()
{
    const unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED) {
        wifiDisconnectedSince = 0;
        return;
    }

    if (wifiDisconnectedSince == 0) {
        wifiDisconnectedSince = now;
        mqttServerResolved = false;
        return;
    }

    if (now - wifiDisconnectedSince >= WIFI_RETRY_INTERVAL_MS) {
        wifiDisconnectedSince = now;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
}

void makeMqttClientId()
{
    // eFuse MAC je uložená v pořadí bajtů: bajt 0 = MAC[0].
    // Spodní 3 bajty MAC jsou OUI výrobce (u všech ESP stejné),
    // unikátní jsou až bajty MAC[3..5] => bity 24..47.
    const uint64_t mac = ESP.getEfuseMac();
    const uint32_t unique =
        (static_cast<uint32_t>((mac >> 24) & 0xFF) << 16) |
        (static_cast<uint32_t>((mac >> 32) & 0xFF) << 8) |
        static_cast<uint32_t>((mac >> 40) & 0xFF);
    snprintf(mqttClientId, sizeof(mqttClientId), "HDO-C3-%06X",
             static_cast<unsigned int>(unique));
}

bool endsWith(const char* text, const char* suffix)
{
    const size_t textLen = strlen(text);
    const size_t suffixLen = strlen(suffix);
    return textLen >= suffixLen &&
           strcasecmp(text + textLen - suffixLen, suffix) == 0;
}

// Běžný DNS v lwIP ".local" adresy spolehlivě nepřeloží, proto se
// použije mDNS dotaz. Výsledná IP se drží až do selhání spojení.
bool resolveMqttServer()
{
    if (mqttServerResolved) return true;

    if (!endsWith(MQTT_HOST, ".local")) {
        mqttClient.setServer(MQTT_HOST, MQTT_PORT);
        mqttServerResolved = true;
        return true;
    }

    if (!mdnsStarted) {
        mdnsStarted = MDNS.begin(mqttClientId);
        if (!mdnsStarted) return false;
    }

    char hostname[sizeof(MQTT_HOST)];
    strncpy(hostname, MQTT_HOST, sizeof(hostname));
    hostname[strlen(hostname) - strlen(".local")] = '\0';

    const IPAddress ip = MDNS.queryHost(hostname, MDNS_QUERY_TIMEOUT_MS);
    if (ip == IPAddress(0, 0, 0, 0)) return false;

    mqttClient.setServer(ip, MQTT_PORT);
    mqttServerResolved = true;
    return true;
}

void connectMQTTIfNeeded()
{
    if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;

    const unsigned long now = millis();
    if (now - lastMqttAttempt < MQTT_RETRY_INTERVAL_MS) return;
    lastMqttAttempt = now;

    if (!resolveMqttServer()) return;

    bool connected = false;

    // PubSubClient provede krátký síťový pokus; opakování řídí loop(),
    // ne nekonečná smyčka.
    if (strlen(MQTT_USER) == 0) {
        connected = mqttClient.connect(
            mqttClientId,
            MQTT_AVAILABILITY_TOPIC, 1, true, "OFFLINE");
    } else {
        connected = mqttClient.connect(
            mqttClientId,
            MQTT_USER,
            MQTT_PASSWORD,
            MQTT_AVAILABILITY_TOPIC, 1, true, "OFFLINE");
    }

    if (connected) {
        publishAvailability("ONLINE");
        publishAll();
        lastReport = millis();
    } else {
        // IP brokera se mohla změnit - příště přeložit znovu.
        mqttServerResolved = false;
    }
}

// Polling s debounce: stav se převezme, až je vstup stabilní
// HDO_DEBOUNCE_MS. Pin se čte v každém průchodu, takže nehrozí
// zaseknutí stavu kvůli zmeškané hraně.
void updateHdo()
{
    const unsigned long now = millis();
    const bool raw = readHdo();

    if (raw != hdoRawState) {
        hdoRawState = raw;
        hdoRawChangeTime = now;
        return;
    }

    if (raw == hdoState || now - hdoRawChangeTime < HDO_DEBOUNCE_MS) return;

    hdoState = raw;
    publishHdo();
    publishState();
}

void refreshOLED()
{
    if (!oledReady) return;

    const unsigned long now = millis();
    if (now - lastOledRefresh < OLED_REFRESH_INTERVAL_MS) return;
    lastOledRefresh = now;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    display.setCursor(0, OLED_HEADER_Y);
    display.println("HDO MONITOR");

    display.setCursor(0, OLED_BODY_Y);
    display.print("WiFi: ");
    if (WiFi.status() == WL_CONNECTED) {
        display.print(WiFi.RSSI());
        display.println(" dBm");
    } else {
        display.println("OFFLINE");
    }

    display.setCursor(0, OLED_BODY_Y + OLED_LINE_HEIGHT);
    display.print("MQTT: ");
    display.println(mqttClient.connected() ? "CONNECTED" : "OFFLINE");

    display.setCursor(0, OLED_BODY_Y + 2 * OLED_LINE_HEIGHT);
    display.print("HDO:  ");
    display.println(hdoState ? "ACTIVE" : "INACTIVE");

    display.setCursor(0, OLED_BODY_Y + 3 * OLED_LINE_HEIGHT);
    display.print("Temp: --.- C");

    display.setCursor(0, OLED_BODY_Y + 4 * OLED_LINE_HEIGHT);
    if (WiFi.status() == WL_CONNECTED) {
        display.print(WiFi.localIP());
    } else {
        display.print("IP: unavailable");
    }

    display.display();
}

void setup()
{
    // Interní pull-up jako záloha k externímu rezistoru.
    pinMode(HDO_PIN, INPUT_PULLUP);
    hdoState = readHdo();
    hdoRawState = hdoState;

    makeMqttClientId();

    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);

    if (oledReady) {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("HDO MONITOR");
        display.println("Starting...");
        display.display();
    }

    mqttClient.setSocketTimeout(1);
    mqttClient.setKeepAlive(30);

    startWiFi();

    lastReport = millis();
}

void loop()
{
    maintainWiFi();
    connectMQTTIfNeeded();

    if (mqttClient.connected()) {
        mqttClient.loop();
    }

    updateHdo();

    const unsigned long now = millis();
    if (now - lastReport >= REPORT_INTERVAL_MS) {
        lastReport = now;
        publishAll();
    }

    refreshOLED();
    delay(1);
}
