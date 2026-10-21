#pragma once

// Volitelný OLED SSD1306 (build flag HDO_OLED=1). Bez něj jsou
// funkce prázdné.

void displaySetup();
void displayMessage(const char* line1, const char* line2);
// Jako displayMessage(), ale hláška vydrží ERROR_HOLD_MS, než ji
// displayRefresh() přepíše stavovou obrazovkou.
void displayHoldMessage(const char* line1, const char* line2);
void displayRefresh(bool mqttConnected);
