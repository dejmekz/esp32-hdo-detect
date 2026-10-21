#include "network.h"

#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <string.h>

#include <backoff.h>
#include <state_json.h>

#include "config.h"
#include "hdo.h"
#include "log.h"
#include "ota.h"
#include "restart.h"
#include "sensor.h"
#include "version.h"

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static bool wifiWasConnected = false;
static bool mqttWasConnected = false;
static bool mqttServerResolved = false;
// Povel restart se provede až v networkLoop(), ne v callbacku
// uvnitř mqttClient.loop().
static bool restartRequested = false;
static Backoff mqttBackoff(MQTT_RETRY_MIN_MS, MQTT_RETRY_MAX_MS);

static char mqttClientId[24];
static char mqttNodeId[24];
static const char* resetReasonName = "unknown";

static unsigned long wifiDisconnectedSince = 0;
static unsigned long lastMqttAttempt = 0;
static unsigned long lastReport = 0;

static void publishAvailability(const char* state)
{
    if (mqttClient.connected()) {
        mqttClient.publish(MQTT_AVAILABILITY_TOPIC, state, true);
    }
}

void publishHdo()
{
    if (!mqttClient.connected()) return;

    mqttClient.publish(MQTT_HDO_TOPIC, hdoLowTariff() ? "ON" : "OFF", true);
}

void publishState()
{
    if (!mqttClient.connected()) return;

    const String ip = WiFi.localIP().toString();

    StateInfo info;
    info.lowTariff = hdoLowTariff();
    info.rssi = WiFi.RSSI();
    // esp_timer je 64bitový, millis() by přetekl po 49 dnech.
    info.uptimeS = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    info.ip = ip.c_str();
    info.resetReason = resetReasonName;
    info.firmware = FW_VERSION;
    info.freeHeap = ESP.getFreeHeap();
    info.minFreeHeap = ESP.getMinFreeHeap();
    info.tariffS = hdoTariffSeconds();
    info.climateValid = sensorFresh();
    info.temperature = sensorTemperature();
    info.humidity = sensorHumidity();

    char payload[320];
    if (!formatStateJson(payload, sizeof(payload), info)) {
        LOG("MQTT: state payload too long");
        return;
    }
    mqttClient.publish(MQTT_STATE_TOPIC, payload, true);
}

static void publishAll()
{
    publishHdo();
    publishState();
}

// Jedna entita pro Home Assistant discovery. entityFields = JSON
// položky specifické pro entitu (bez složených závorek); společné
// položky (unique_id, availability, device) se doplní zde.
static void publishDiscoveryEntity(const char* component, const char* objectId,
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

    snprintf(fields, sizeof(fields),
             "\"name\":\"Free heap\",\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.heap }}\","
             "\"unit_of_measurement\":\"B\","
             "\"state_class\":\"measurement\","
             "\"entity_category\":\"diagnostic\"",
             MQTT_STATE_TOPIC);
    publishDiscoveryEntity("sensor", "heap", fields);

    snprintf(fields, sizeof(fields),
             "\"name\":\"Restart\",\"command_topic\":\"%s\","
             "\"payload_press\":\"restart\","
             "\"device_class\":\"restart\","
             "\"entity_category\":\"config\"",
             MQTT_COMMAND_TOPIC);
    publishDiscoveryEntity("button", "restart", fields);

    if (sensorPresent()) {
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

void mqttGoOffline()
{
    publishAvailability("OFFLINE");
    mqttClient.disconnect();
}

static void onMqttMessage(char* topic, uint8_t* payload, unsigned int length)
{
    if (strcmp(topic, MQTT_COMMAND_TOPIC) != 0) return;
    // Prázdná zpráva = smazání retained povelu, nic nedělat.
    if (length == 0) return;

    char command[16];
    if (length >= sizeof(command)) {
        LOG("MQTT: command too long, ignored");
        return;
    }
    memcpy(command, payload, length);
    command[length] = '\0';

    if (strcasecmp(command, "restart") == 0) {
        LOG("MQTT: restart command received");
        restartRequested = true;
    } else {
        LOG("MQTT: unknown command \"%s\"", command);
    }
}

static void handleRestartRequest()
{
    if (!restartRequested) return;

    // Retained povel smazat, jinak by se zařízení po každém připojení
    // k brokeru restartovalo znovu.
    mqttClient.publish(MQTT_COMMAND_TOPIC, nullptr, 0, true);
    mqttGoOffline();
    LOG("Restarting on command");
    delay(100);
    restartOnCommand();
}

static void mqttAttemptFailed()
{
    mqttBackoff.failed();
    LOG("MQTT: next attempt in %lu s",
        static_cast<unsigned long>(mqttBackoff.delayMs() / 1000));
}

static void startWiFi()
{
    LOG("WiFi: connecting to \"%s\"", WIFI_SSID);
    WiFi.persistent(false);
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_TX_POWER);
    // Napájení ze sítě: bez úsporného režimu Wi-Fi zařízení spolehlivěji
    // odpovídá na mDNS, OTA pozvánky a MQTT keepalive.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Běžné výpadky řeší automatický reconnect knihovny. Ruční restart
// Wi-Fi jen tehdy, když se nepřipojí ani po WIFI_RETRY_INTERVAL_MS.
static void maintainWiFi()
{
    const unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            LOG("WiFi: connected, IP %s, RSSI %d dBm",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
            if (!otaReady()) otaSetup();
            // Odstup z doby bez Wi-Fi neplatí, MQTT zkusit hned.
            mqttBackoff.reset();
            lastMqttAttempt = millis() - MQTT_RETRY_MIN_MS;
        }
        wifiDisconnectedSince = 0;
        return;
    }

    if (wifiWasConnected) {
        wifiWasConnected = false;
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

static void makeMqttClientId()
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

static bool endsWith(const char* text, const char* suffix)
{
    const size_t textLen = strlen(text);
    const size_t suffixLen = strlen(suffix);
    return textLen >= suffixLen &&
           strcasecmp(text + textLen - suffixLen, suffix) == 0;
}

// Běžný DNS v lwIP ".local" adresy spolehlivě nepřeloží, proto se
// použije mDNS dotaz. Výsledná IP se drží až do selhání spojení.
static bool resolveMqttServer()
{
    if (mqttServerResolved) return true;

    if (!endsWith(MQTT_HOST, ".local")) {
        mqttClient.setServer(MQTT_HOST, MQTT_PORT);
        mqttServerResolved = true;
        return true;
    }

    if (!otaReady()) return false;

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

static void connectMQTTIfNeeded()
{
    if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;

    const unsigned long now = millis();
    if (now - lastMqttAttempt < mqttBackoff.delayMs()) return;
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
        mqttBackoff.reset();
        if (!mqttClient.subscribe(MQTT_COMMAND_TOPIC)) {
            LOG("MQTT: subscribe to %s failed", MQTT_COMMAND_TOPIC);
        }
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
        if (mqttBackoff.failures() % MQTT_RESOLVE_AFTER_FAILURES == 0) {
            mqttServerResolved = false;
        }
    }
}

void networkSetup(const char* resetReason)
{
    resetReasonName = resetReason;
    makeMqttClientId();
    LOG("MQTT: client ID %s", mqttClientId);

    wifiClient.setTimeout(MQTT_CONNECT_TIMEOUT_S);
    mqttClient.setSocketTimeout(1);
    mqttClient.setKeepAlive(30);
    mqttClient.setCallback(onMqttMessage);
    if (!mqttClient.setBufferSize(MQTT_BUFFER_SIZE)) {
        LOG("MQTT: cannot allocate %u B buffer",
            static_cast<unsigned int>(MQTT_BUFFER_SIZE));
    }

    startWiFi();
    lastReport = millis();
}

void networkLoop()
{
    maintainWiFi();
    connectMQTTIfNeeded();

    if (mqttClient.connected()) {
        mqttClient.loop();
    }
    handleRestartRequest();

    const bool connected = mqttClient.connected();
    if (mqttWasConnected && !connected) {
        LOG("MQTT: disconnected, state %d", mqttClient.state());
    }
    mqttWasConnected = connected;

    const unsigned long now = millis();
    if (now - lastReport >= REPORT_INTERVAL_MS) {
        lastReport = now;
        publishAll();
    }
}

bool wifiConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

bool mqttConnected()
{
    return mqttClient.connected();
}
