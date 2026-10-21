#pragma once

// Wi-Fi, MQTT (vč. překladu .local adresy přes mDNS), publikování
// stavu a volitelné Home Assistant discovery.

// resetReason se publikuje v hdo/state.
void networkSetup(const char* resetReason);

// Údržba Wi-Fi a MQTT a pravidelný report, volat v každém průchodu
// smyčkou.
void networkLoop();

bool wifiConnected();
bool mqttConnected();

// Odpojení před OTA. Čisté odpojení LWT nespustí, proto se OFFLINE
// publikuje ručně.
void mqttGoOffline();

void publishHdo();
void publishState();
void publishDiscovery();
