#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <Wire.h>

#include "config.h"

#if HDO_OLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif

// Log s časem od startu v ms, např. "[   12345] WiFi: connected".
#define LOG(fmt, ...)                                                   \
    do {                                                                \
        if (SERIAL_LOGGING) {                                           \
            Serial.printf("[%8lu] " fmt "\n", millis(), ##__VA_ARGS__); \
        }                                                               \
    } while (0)

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
#if HDO_OLED
Adafruit_SSD1306 display(128, 64, &Wire, OLED_RESET_PIN);
#endif

// hdoState = odfiltrovaný (debounced) stav; jediný zdroj pravdy
// pro MQTT, OLED i LED.
bool hdoState = false;
bool hdoRawState = false;
bool oledReady = false;
bool wifiConnected = false;
bool mqttWasConnected = false;
// ArduinoOTA.begin() spouští i mDNS; MDNS.begin() nelze volat dvakrát.
bool networkServicesStarted = false;
bool mqttServerResolved = false;
// Hláška na OLED (např. OTA ERROR), kterou refreshOLED() zatím nepřepíše.
bool oledMessageHeld = false;
// LED bliká fialově po chybě OTA, dokud neuplyne OLED_MESSAGE_HOLD_MS.
bool ledOtaErrorHeld = false;
int lastOtaPercent = -1;
uint8_t mqttFailures = 0;
wifi_power_t normalTxPower = WIFI_POWER_19_5dBm;

char mqttClientId[24];
char mqttNodeId[24];
// Nastaví setup(); publikuje se v hdo/state.
const char* resetReasonName = "unknown";

// Čas sestavení - v HA (sw_version) a v logu je vidět, zda OTA prošla.
constexpr char FIRMWARE_BUILD[] = __DATE__ " " __TIME__;

unsigned long hdoRawChangeTime = 0;
unsigned long lastReport = 0;
unsigned long wifiDisconnectedSince = 0;
unsigned long lastMqttAttempt = 0;
unsigned long mqttRetryDelay = MQTT_RETRY_MIN_MS;
unsigned long lastOledRefresh = 0;
unsigned long oledMessageSince = 0;
unsigned long ledOtaErrorSince = 0;

// DHT20: měření běží na pozadí (spuštění -> po 80 ms čtení), smyčka
// na převod nečeká.
bool sensorReady = false;
bool sensorMeasuring = false;
bool sensorHasValue = false;
float sensorTemperature = NAN;
float sensorHumidity = NAN;
unsigned long sensorMeasureStart = 0;
unsigned long sensorLastValue = 0;

enum class LedColor : uint8_t { Off, Red, Green, Blue, Magenta };
LedColor ledColor = LedColor::Off;
bool ledColorSet = false;

bool readHdo()
{
    return digitalRead(HDO_PIN) == HDO_ACTIVE_LEVEL;
}

// HDO aktivní = nízký tarif (NT), jinak vysoký tarif (VT).
const char* tariffText()
{
    return hdoState ? "NT" : "VT";
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


// Platná a ne příliš stará hodnota z DHT20.
bool sensorValueFresh()
{
    return sensorHasValue && millis() - sensorLastValue < SENSOR_STALE_MS;
}

// Stav + diagnostika. Klíč "hdo" zůstává kvůli kompatibilitě.
// Teplota a vlhkost jen tehdy, když je k dispozici aktuální měření.
void publishState()
{
    if (!mqttClient.connected()) return;

    // esp_timer je 64bitový, millis() by přetekl po 49 dnech.
    const unsigned long uptimeS =
        static_cast<unsigned long>(esp_timer_get_time() / 1000000);

    char climate[48] = "";
    if (sensorValueFresh()) {
        snprintf(climate, sizeof(climate),
                 ",\"temperature\":%.1f,\"humidity\":%.1f",
                 sensorTemperature, sensorHumidity);
    }

    char payload[224];
    snprintf(payload, sizeof(payload),
             "{\"hdo\":%s,\"rssi\":%d,\"uptime\":%lu,\"ip\":\"%s\","
             "\"reset\":\"%s\"%s}",
             hdoState ? "true" : "false",
             static_cast<int>(WiFi.RSSI()),
             uptimeS,
             WiFi.localIP().toString().c_str(),
             resetReasonName,
             climate);
    mqttClient.publish(MQTT_STATE_TOPIC, payload, true);
}

// Jedna entita pro Home Assistant discovery. entityFields = JSON
// položky specifické pro entitu (bez složených závorek); společné
// položky (unique_id, availability, device) se doplní zde.
void publishDiscoveryEntity(const char* component, const char* objectId,
                            const char* entityFields)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
             HA_DISCOVERY_PREFIX, component, mqttNodeId, objectId);

    char payload[MQTT_BUFFER_SIZE - sizeof(topic) - 8];
    const int len = snprintf(
        payload, sizeof(payload),
        "{%s,"
        "\"unique_id\":\"%s_%s\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"ONLINE\","
        "\"payload_not_available\":\"OFFLINE\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"HDO monitor\","
        "\"model\":\"ESP32-C3\",\"sw_version\":\"%s\"}}",
        entityFields, mqttNodeId, objectId, MQTT_AVAILABILITY_TOPIC,
        mqttNodeId, FIRMWARE_BUILD);

    if (len < 0 || static_cast<size_t>(len) >= sizeof(payload)) {
        LOG("MQTT: discovery payload for %s too long", objectId);
        return;
    }
    if (!mqttClient.publish(topic, payload, true)) {
        LOG("MQTT: discovery publish of %s failed", objectId);
    }
}

void publishDiscovery()
{
    if (!HA_DISCOVERY || !mqttClient.connected()) return;

    char fields[256];

    snprintf(fields, sizeof(fields),
             "\"name\":\"HDO\",\"state_topic\":\"%s\","
             "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
             "\"icon\":\"mdi:transmission-tower\"",
             MQTT_HDO_TOPIC);
    publishDiscoveryEntity("binary_sensor", "hdo", fields);

    snprintf(fields, sizeof(fields),
             "\"name\":\"Wi-Fi signal\",\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.rssi }}\","
             "\"unit_of_measurement\":\"dBm\","
             "\"device_class\":\"signal_strength\","
             "\"state_class\":\"measurement\","
             "\"entity_category\":\"diagnostic\"",
             MQTT_STATE_TOPIC);
    publishDiscoveryEntity("sensor", "rssi", fields);

    snprintf(fields, sizeof(fields),
             "\"name\":\"Uptime\",\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.uptime }}\","
             "\"unit_of_measurement\":\"s\","
             "\"device_class\":\"duration\","
             "\"entity_category\":\"diagnostic\"",
             MQTT_STATE_TOPIC);
    publishDiscoveryEntity("sensor", "uptime", fields);

    snprintf(fields, sizeof(fields),
             "\"name\":\"Reset reason\",\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.reset }}\","
             "\"icon\":\"mdi:restart-alert\","
             "\"entity_category\":\"diagnostic\"",
             MQTT_STATE_TOPIC);
    publishDiscoveryEntity("sensor", "reset", fields);

    snprintf(fields, sizeof(fields),
             "\"name\":\"IP address\",\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.ip }}\","
             "\"icon\":\"mdi:ip-network\","
             "\"entity_category\":\"diagnostic\"",
             MQTT_STATE_TOPIC);
    publishDiscoveryEntity("sensor", "ip", fields);

    if (sensorReady) {
        snprintf(fields, sizeof(fields),
                 "\"name\":\"Temperature\",\"state_topic\":\"%s\","
                 "\"value_template\":\"{{ value_json.temperature }}\","
                 "\"unit_of_measurement\":\"°C\","
                 "\"device_class\":\"temperature\","
                 "\"state_class\":\"measurement\"",
                 MQTT_STATE_TOPIC);
        publishDiscoveryEntity("sensor", "temperature", fields);

        snprintf(fields, sizeof(fields),
                 "\"name\":\"Humidity\",\"state_topic\":\"%s\","
                 "\"value_template\":\"{{ value_json.humidity }}\","
                 "\"unit_of_measurement\":\"%%\","
                 "\"device_class\":\"humidity\","
                 "\"state_class\":\"measurement\"",
                 MQTT_STATE_TOPIC);
        publishDiscoveryEntity("sensor", "humidity", fields);
    }

    LOG("MQTT: Home Assistant discovery published");
}

void publishAll()
{
    publishHdo();
    publishState();
}

void showOledMessage(const char* line1, const char* line2)
{
#if HDO_OLED
    if (!oledReady) return;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, OLED_HEADER_Y);
    display.print(line1);
    display.setCursor(0, OLED_BODY_Y);
    display.print(line2);
    display.display();
#endif
}

// Jako showOledMessage(), ale hláška vydrží OLED_MESSAGE_HOLD_MS,
// než ji refreshOLED() přepíše stavovou obrazovkou.
void showHeldOledMessage(const char* line1, const char* line2)
{
    showOledMessage(line1, line2);
    oledMessageHeld = true;
    oledMessageSince = millis();
}

void writeLedChannel(uint8_t channel, uint8_t brightness)
{
    // Společná anoda: svítí při LOW => obrácená střída.
    // ledcWrite(255) při 8 bitech dává trvale HIGH.
    ledcWrite(channel, LED_COMMON_ANODE ? 255 - brightness : brightness);
}

void setLed(LedColor color)
{
    if (ledColorSet && color == ledColor) return;
    ledColor = color;
    ledColorSet = true;

    const bool red = color == LedColor::Red || color == LedColor::Magenta;
    const bool green = color == LedColor::Green;
    const bool blue = color == LedColor::Blue || color == LedColor::Magenta;
    writeLedChannel(LED_RED_CHANNEL, red ? LED_RED_BRIGHTNESS : 0);
    writeLedChannel(LED_GREEN_CHANNEL, green ? LED_GREEN_BRIGHTNESS : 0);
    writeLedChannel(LED_BLUE_CHANNEL, blue ? LED_BLUE_BRIGHTNESS : 0);
}

void setupLed()
{
    ledcSetup(LED_RED_CHANNEL, LED_PWM_FREQ_HZ, LED_PWM_RESOLUTION);
    ledcSetup(LED_GREEN_CHANNEL, LED_PWM_FREQ_HZ, LED_PWM_RESOLUTION);
    ledcSetup(LED_BLUE_CHANNEL, LED_PWM_FREQ_HZ, LED_PWM_RESOLUTION);
    ledcAttachPin(LED_RED_PIN, LED_RED_CHANNEL);
    ledcAttachPin(LED_GREEN_PIN, LED_GREEN_CHANNEL);
    ledcAttachPin(LED_BLUE_PIN, LED_BLUE_CHANNEL);
    // Modrá = zařízení startuje (do prvního průchodu smyčkou).
    setLed(LedColor::Blue);
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
        setLed(LedColor::Blue);
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        // Celý upload běží uvnitř ArduinoOTA.handle(), loop() mezitím
        // neproběhne - watchdog se proto resetuje po každém bloku dat.
        esp_task_wdt_reset();

        const int percent = total > 0 ? progress * 100 / total : 0;
        if (percent == lastOtaPercent) return;
        lastOtaPercent = percent;

        if (percent % 10 == 0) LOG("OTA: %d %%", percent);
        // loop() neběží, LED se přepíná přímo zde: modře problikává.
        setLed(percent % 2 == 0 ? LedColor::Blue : LedColor::Off);

        char line[24];
        snprintf(line, sizeof(line), "Progress: %d %%", percent);
        showOledMessage("OTA UPDATE", line);
    });

    ArduinoOTA.onEnd([]() {
        LOG("OTA: done, restarting");
        showOledMessage("OTA UPDATE", "Done, restarting...");
        setLed(LedColor::Blue);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        LOG("OTA: error %u (%s)", error, otaErrorText(error));
        WiFi.setTxPower(normalTxPower);
        showHeldOledMessage("OTA ERROR", otaErrorText(error));
        ledOtaErrorHeld = true;
        ledOtaErrorSince = millis();
    });

    ArduinoOTA.begin();
    networkServicesStarted = true;
    LOG("OTA: ready at %s.local%s", DEVICE_HOSTNAME,
        strlen(OTA_PASSWORD) > 0 ? "" : " (NO PASSWORD)");
}

void resetMqttBackoff()
{
    mqttFailures = 0;
    mqttRetryDelay = MQTT_RETRY_MIN_MS;
}

void mqttAttemptFailed()
{
    if (mqttFailures < UINT8_MAX) mqttFailures++;
    mqttRetryDelay = min(mqttRetryDelay * 2, MQTT_RETRY_MAX_MS);
    LOG("MQTT: next attempt in %lu s", mqttRetryDelay / 1000);
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
            // Odstup z doby bez Wi-Fi neplatí, MQTT zkusit hned.
            resetMqttBackoff();
            lastMqttAttempt = millis() - MQTT_RETRY_MIN_MS;
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
    // Node ID pro HA discovery topic smí obsahovat jen [a-zA-Z0-9_-].
    snprintf(mqttNodeId, sizeof(mqttNodeId), "hdo_c3_%06x",
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
    if (now - lastMqttAttempt < mqttRetryDelay) return;
    lastMqttAttempt = now;

    if (!resolveMqttServer()) {
        mqttAttemptFailed();
        return;
    }

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
        resetMqttBackoff();
        publishAvailability("ONLINE");
        publishDiscovery();
        publishAll();
        lastReport = millis();
    } else {
        // Kódy viz PubSubClient.h (-2 = síť nedostupná, 4/5 = přihlášení).
        LOG("MQTT: connect failed, state %d", mqttClient.state());
        mqttAttemptFailed();
        // IP brokera se mohla změnit - po několika neúspěších přeložit
        // znovu. Ne po každém: mDNS dotaz blokuje smyčku až 2 s.
        if (mqttFailures % MQTT_RESOLVE_AFTER_FAILURES == 0) {
            mqttServerResolved = false;
        }
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
    LOG("HDO: tariff %s", tariffText());
    publishHdo();
    publishState();
}

// CRC-8 DHT20: polynom 0x31, počáteční hodnota 0xFF.
uint8_t dht20Crc(const uint8_t* data, size_t len)
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

bool dht20Write(const uint8_t* data, size_t len)
{
    Wire.beginTransmission(DHT20_I2C_ADDRESS);
    Wire.write(data, len);
    return Wire.endTransmission() == 0;
}

// Stavový bajt, -1 = senzor neodpovídá.
int dht20ReadStatus()
{
    if (Wire.requestFrom(DHT20_I2C_ADDRESS, static_cast<uint8_t>(1)) != 1) {
        return -1;
    }
    return Wire.read();
}

void setupSensor()
{
    // Senzor potřebuje po zapnutí napájení 100 ms.
    while (millis() < DHT20_POWERUP_MS) {
        delay(10);
    }

    const int status = dht20ReadStatus();
    if (status < 0) {
        LOG("DHT20: not found at 0x%02X", DHT20_I2C_ADDRESS);
        return;
    }
    // Bity 3 a 4 = kalibrace načtená; jinak inicializace (příkaz 0xBE).
    if ((status & 0x18) != 0x18) {
        static const uint8_t init[] = {0xBE, 0x08, 0x00};
        dht20Write(init, sizeof(init));
        delay(10);
        LOG("DHT20: status 0x%02X, initialized", status);
    }

    sensorReady = true;
    // První měření hned v prvním průchodu smyčkou.
    sensorMeasureStart = millis() - SENSOR_READ_INTERVAL_MS;
    LOG("DHT20: found at 0x%02X", DHT20_I2C_ADDRESS);
}

// Spustí měření každých SENSOR_READ_INTERVAL_MS a výsledek přečte
// až po DHT20_MEASURE_MS, takže smyčku neblokuje.
void updateSensor()
{
    if (!sensorReady) return;

    const unsigned long now = millis();

    if (!sensorMeasuring) {
        if (now - sensorMeasureStart < SENSOR_READ_INTERVAL_MS) return;
        sensorMeasureStart = now;
        static const uint8_t trigger[] = {0xAC, 0x33, 0x00};
        if (dht20Write(trigger, sizeof(trigger))) {
            sensorMeasuring = true;
        } else {
            LOG("DHT20: trigger failed");
        }
        return;
    }

    if (now - sensorMeasureStart < DHT20_MEASURE_MS) return;

    uint8_t data[7];
    const uint8_t count = Wire.requestFrom(DHT20_I2C_ADDRESS,
                                           static_cast<uint8_t>(sizeof(data)));
    for (uint8_t i = 0; i < count && i < sizeof(data); i++) {
        data[i] = Wire.read();
    }

    if (count == sizeof(data) && (data[0] & 0x80)) {
        // Převod ještě běží - zkusit v dalším průchodu, nejvýš do timeoutu.
        if (now - sensorMeasureStart < DHT20_MEASURE_TIMEOUT_MS) return;
        LOG("DHT20: measurement timeout");
        sensorMeasuring = false;
        return;
    }
    sensorMeasuring = false;

    if (count != sizeof(data)) {
        LOG("DHT20: read failed (%u B)", count);
        return;
    }
    if (dht20Crc(data, 6) != data[6]) {
        LOG("DHT20: CRC error");
        return;
    }

    // 20 bitů vlhkost, 20 bitů teplota (rozsah -50..150 °C).
    const uint32_t rawHumidity = (static_cast<uint32_t>(data[1]) << 12) |
                                 (static_cast<uint32_t>(data[2]) << 4) |
                                 (data[3] >> 4);
    const uint32_t rawTemperature =
        (static_cast<uint32_t>(data[3] & 0x0F) << 16) |
        (static_cast<uint32_t>(data[4]) << 8) | data[5];
    sensorHumidity = rawHumidity * 100.0f / 1048576.0f;
    sensorTemperature = rawTemperature * 200.0f / 1048576.0f - 50.0f;

    if (!sensorHasValue) {
        LOG("DHT20: %.1f °C, %.1f %%", sensorTemperature, sensorHumidity);
    }
    sensorHasValue = true;
    sensorLastValue = now;
}

// NT = zelená, VT = červená. Svítí trvale, když je Wi-Fi i MQTT
// připojené; jinak bliká (rychle bez Wi-Fi, pomalu bez MQTT).
// Po chybě OTA rychle bliká fialově.
void refreshLed()
{
    const unsigned long now = millis();

    if (ledOtaErrorHeld) {
        if (now - ledOtaErrorSince < OLED_MESSAGE_HOLD_MS) {
            const bool on = (now / LED_BLINK_FAST_MS) % 2 == 0;
            setLed(on ? LedColor::Magenta : LedColor::Off);
            return;
        }
        ledOtaErrorHeld = false;
    }

    unsigned long blinkMs = 0;
    if (WiFi.status() != WL_CONNECTED) {
        blinkMs = LED_BLINK_FAST_MS;
    } else if (!mqttClient.connected()) {
        blinkMs = LED_BLINK_SLOW_MS;
    }

    const LedColor tariff = hdoState ? LedColor::Green : LedColor::Red;
    const bool on = blinkMs == 0 || (now / blinkMs) % 2 == 0;
    setLed(on ? tariff : LedColor::Off);
}

void refreshOLED()
{
#if HDO_OLED
    if (!oledReady) return;

    const unsigned long now = millis();
    if (oledMessageHeld) {
        if (now - oledMessageSince < OLED_MESSAGE_HOLD_MS) return;
        oledMessageHeld = false;
    }
    if (now - lastOledRefresh < OLED_REFRESH_INTERVAL_MS) return;
    lastOledRefresh = now;

    // Posun proti vypálení: cyklicky po OLED_SHIFT_INTERVAL_MS.
    // Nejdelší řádek má 15 znaků = 90 px, posun X do 6 px se vejde.
    static const int8_t shifts[][2] = {
        {0, 0}, {2, 1}, {4, 2}, {6, 1}, {4, 0}, {2, -1}, {4, -2}, {6, -1},
    };
    const size_t shiftCount = sizeof(shifts) / sizeof(shifts[0]);
    const size_t shift = (now / OLED_SHIFT_INTERVAL_MS) % shiftCount;
    const int16_t x = shifts[shift][0];
    const int16_t y = shifts[shift][1];

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    display.setCursor(x, OLED_HEADER_Y + y);
    display.println("HDO MONITOR");

    display.setCursor(x, OLED_BODY_Y + y);
    display.print("WiFi: ");
    if (WiFi.status() == WL_CONNECTED) {
        display.print(WiFi.RSSI());
        display.println(" dBm");
    } else {
        display.println("OFFLINE");
    }

    display.setCursor(x, OLED_BODY_Y + OLED_LINE_HEIGHT + y);
    display.print("MQTT: ");
    display.println(mqttClient.connected() ? "CONNECTED" : "OFFLINE");

    display.setCursor(x, OLED_BODY_Y + 2 * OLED_LINE_HEIGHT + y);
    display.print("Tarif: ");
    display.println(tariffText());

    display.setCursor(x, OLED_BODY_Y + 3 * OLED_LINE_HEIGHT + y);
    if (WiFi.status() == WL_CONNECTED) {
        display.print(WiFi.localIP());
    } else {
        display.print("IP: unavailable");
    }

    display.display();
#endif
}

void setupOLED()
{
#if HDO_OLED
    oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);
    if (!oledReady) LOG("OLED: not found at 0x%02X", OLED_I2C_ADDRESS);

    if (oledReady) {
        display.ssd1306_command(SSD1306_SETCONTRAST);
        display.ssd1306_command(OLED_CONTRAST);
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("HDO MONITOR");
        display.println("Starting...");
        display.display();
    }
#endif
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

void setupWatchdog()
{
    // Arduino už TWDT spouští (5 s, jen idle task); v IDF 4.4 volání
    // esp_task_wdt_init() změní timeout existujícího watchdogu.
    // panic = true => restart s důvodem "task watchdog".
    esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
    esp_task_wdt_add(nullptr);  // hlídat i loopTask (aktuální task)
    LOG("Watchdog: %lu s", static_cast<unsigned long>(WATCHDOG_TIMEOUT_S));
}

void setup()
{
    setupLed();
    setupSerial();
    setupWatchdog();

    // Interní pull-up jako záloha k externímu rezistoru.
    pinMode(HDO_PIN, INPUT_PULLUP);
    hdoState = readHdo();
    hdoRawState = hdoState;

    makeMqttClientId();

    const esp_reset_reason_t resetReason = esp_reset_reason();
    resetReasonName = resetReasonText(resetReason);
    LOG("HDO monitor start, build %s, reset reason %d (%s), client ID %s",
        FIRMWARE_BUILD, static_cast<int>(resetReason), resetReasonName,
        mqttClientId);
    LOG("HDO: initial tariff %s", tariffText());

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    setupSensor();
    setupOLED();

    wifiClient.setTimeout(MQTT_CONNECT_TIMEOUT_S);
    mqttClient.setSocketTimeout(1);
    mqttClient.setKeepAlive(30);
    if (!mqttClient.setBufferSize(MQTT_BUFFER_SIZE)) {
        LOG("MQTT: cannot allocate %u B buffer",
            static_cast<unsigned int>(MQTT_BUFFER_SIZE));
    }

    startWiFi();

    lastReport = millis();
}

void loop()
{
    esp_task_wdt_reset();

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
    updateSensor();

    const unsigned long now = millis();
    if (now - lastReport >= REPORT_INTERVAL_MS) {
        lastReport = now;
        publishAll();
    }

    refreshLed();
    refreshOLED();
    delay(1);
}
