#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_system.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

// Log s časem od startu v ms, např. "[   12345] WiFi: connected".
#define LOG(fmt, ...)                                                   \
    do {                                                                \
        if (SERIAL_LOGGING) {                                           \
            Serial.printf("[%8lu] " fmt "\n", millis(), ##__VA_ARGS__); \
        }                                                               \
    } while (0)

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Adafruit_SSD1306 display(128, 64, &Wire, OLED_RESET_PIN);

// hdoState = odfiltrovaný (debounced) stav; jediný zdroj pravdy
// pro MQTT i OLED.
bool hdoState = false;
bool hdoRawState = false;
bool oledReady = false;
bool wifiConnected = false;
bool mqttWasConnected = false;
// ArduinoOTA.begin() spouští i mDNS; MDNS.begin() nelze volat dvakrát.
bool networkServicesStarted = false;
bool mqttServerResolved = false;
int lastOtaPercent = -1;
wifi_power_t normalTxPower = WIFI_POWER_19_5dBm;

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


void publishState()
{
    if (!mqttClient.connected()) return;

    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"hdo\":%s}",
             hdoState ? "true" : "false");
    mqttClient.publish(MQTT_STATE_TOPIC, payload, true);
}

void publishAll()
{
    publishHdo();
    publishState();
}

void showOledMessage(const char* line1, const char* line2)
{
    if (!oledReady) return;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, OLED_HEADER_Y);
    display.print(line1);
    display.setCursor(0, OLED_BODY_Y);
    display.print(line2);
    display.display();
}

const char* otaErrorText(ota_error_t error)
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

const char* resetReasonText(esp_reset_reason_t reason)
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

void setupOTA()
{
    ArduinoOTA.setHostname(DEVICE_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) {
        ArduinoOTA.setPassword(OTA_PASSWORD);
    }

    ArduinoOTA.onStart([]() {
        LOG("OTA: start (%s)",
            ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
        lastOtaPercent = -1;
        // Nižší výkon = nižší proudové špičky => bez brownout resetu.
        normalTxPower = WiFi.getTxPower();
        WiFi.setTxPower(OTA_WIFI_TX_POWER);
        // Čisté odpojení LWT nespustí, proto OFFLINE ručně.
        publishAvailability("OFFLINE");
        mqttClient.disconnect();
        showOledMessage("OTA UPDATE", "Starting...");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        const int percent = total > 0 ? progress * 100 / total : 0;
        if (percent == lastOtaPercent) return;
        lastOtaPercent = percent;

        if (percent % 10 == 0) LOG("OTA: %d %%", percent);

        char line[24];
        snprintf(line, sizeof(line), "Progress: %d %%", percent);
        showOledMessage("OTA UPDATE", line);
    });

    ArduinoOTA.onEnd([]() {
        LOG("OTA: done, restarting");
        showOledMessage("OTA UPDATE", "Done, restarting...");
    });

    ArduinoOTA.onError([](ota_error_t error) {
        LOG("OTA: error %u (%s)", error, otaErrorText(error));
        WiFi.setTxPower(normalTxPower);
        showOledMessage("OTA ERROR", otaErrorText(error));
    });

    ArduinoOTA.begin();
    networkServicesStarted = true;
    LOG("OTA: ready at %s.local%s", DEVICE_HOSTNAME,
        strlen(OTA_PASSWORD) > 0 ? "" : " (NO PASSWORD)");
}

void startWiFi()
{
    LOG("WiFi: connecting to \"%s\"", WIFI_SSID);
    WiFi.persistent(false);
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Běžné výpadky řeší automatický reconnect knihovny. Ruční restart
// Wi-Fi jen tehdy, když se nepřipojí ani po WIFI_RETRY_INTERVAL_MS.
void maintainWiFi()
{
    const unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiConnected) {
            wifiConnected = true;
            LOG("WiFi: connected, IP %s, RSSI %d dBm",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
            if (!networkServicesStarted) setupOTA();
        }
        wifiDisconnectedSince = 0;
        return;
    }

    if (wifiConnected) {
        wifiConnected = false;
        LOG("WiFi: disconnected (status %d)", WiFi.status());
    }

    if (wifiDisconnectedSince == 0) {
        wifiDisconnectedSince = now;
        mqttServerResolved = false;
        return;
    }

    if (now - wifiDisconnectedSince >= WIFI_RETRY_INTERVAL_MS) {
        LOG("WiFi: not connected for %lu s, restarting WiFi",
            WIFI_RETRY_INTERVAL_MS / 1000);
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

    if (!networkServicesStarted) return false;

    char hostname[sizeof(MQTT_HOST)];
    strncpy(hostname, MQTT_HOST, sizeof(hostname));
    hostname[strlen(hostname) - strlen(".local")] = '\0';

    const IPAddress ip = MDNS.queryHost(hostname, MDNS_QUERY_TIMEOUT_MS);
    if (ip == IPAddress(0, 0, 0, 0)) {
        LOG("MQTT: mDNS lookup of %s failed", MQTT_HOST);
        return false;
    }

    LOG("MQTT: %s resolved to %s", MQTT_HOST, ip.toString().c_str());
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
        LOG("MQTT: connected as %s", mqttClientId);
        publishAvailability("ONLINE");
        publishAll();
        lastReport = millis();
    } else {
        // Kódy viz PubSubClient.h (-2 = síť nedostupná, 4/5 = přihlášení).
        LOG("MQTT: connect failed, state %d", mqttClient.state());
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
    LOG("HDO: %s", hdoState ? "ACTIVE" : "INACTIVE");
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
    if (WiFi.status() == WL_CONNECTED) {
        display.print(WiFi.localIP());
    } else {
        display.print("IP: unavailable");
    }

    display.display();
}

void setupSerial()
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

void setup()
{
    setupSerial();

    // Interní pull-up jako záloha k externímu rezistoru.
    pinMode(HDO_PIN, INPUT_PULLUP);
    hdoState = readHdo();
    hdoRawState = hdoState;

    makeMqttClientId();

    const esp_reset_reason_t resetReason = esp_reset_reason();
    LOG("HDO monitor start, reset reason %d (%s), client ID %s",
        static_cast<int>(resetReason), resetReasonText(resetReason),
        mqttClientId);
    LOG("HDO: initial state %s", hdoState ? "ACTIVE" : "INACTIVE");

    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);
    if (!oledReady) LOG("OLED: not found at 0x%02X", OLED_I2C_ADDRESS);

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

    if (networkServicesStarted) {
        ArduinoOTA.handle();
    }

    connectMQTTIfNeeded();

    if (mqttClient.connected()) {
        mqttClient.loop();
    }

    const bool mqttConnected = mqttClient.connected();
    if (mqttWasConnected && !mqttConnected) {
        LOG("MQTT: disconnected, state %d", mqttClient.state());
    }
    mqttWasConnected = mqttConnected;

    updateHdo();

    const unsigned long now = millis();
    if (now - lastReport >= REPORT_INTERVAL_MS) {
        lastReport = now;
        publishAll();
    }

    refreshOLED();
    delay(1);
}
