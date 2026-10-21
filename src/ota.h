#pragma once

// OTA aktualizace přes Wi-Fi (ArduinoOTA).

// Volat po prvním připojení k Wi-Fi. Spouští i mDNS responder.
void otaSetup();

// true = otaSetup() proběhl, mDNS běží (MDNS.begin() nelze volat dvakrát).
bool otaReady();

// Volat v každém průchodu smyčkou; celý upload běží uvnitř.
void otaHandle();
