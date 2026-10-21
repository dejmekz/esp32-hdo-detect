#include "display.h"

#include "config.h"

#if HDO_OLED

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Wire.h>

#include "hdo.h"
#include "log.h"

// Knihovna při každém přenosu přepne I2C na 400 kHz a pak vrátí 100 kHz.
static Adafruit_SSD1306 display(128, 64, &Wire, OLED_RESET_PIN);
static bool displayReady = false;
// Hláška (např. OTA ERROR), kterou displayRefresh() zatím nepřepíše.
static bool messageHeld = false;
static unsigned long messageSince = 0;
static unsigned long lastRefresh = 0;

void displaySetup()
{
    displayReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);
    if (!displayReady) {
        LOG("OLED: not found at 0x%02X", OLED_I2C_ADDRESS);
        return;
    }

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

void displayMessage(const char* line1, const char* line2)
{
    if (!displayReady) return;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, OLED_HEADER_Y);
    display.print(line1);
    display.setCursor(0, OLED_BODY_Y);
    display.print(line2);
    display.display();
}

void displayHoldMessage(const char* line1, const char* line2)
{
    displayMessage(line1, line2);
    messageHeld = true;
    messageSince = millis();
}

void displayRefresh(bool mqttConnected)
{
    if (!displayReady) return;

    const unsigned long now = millis();
    if (messageHeld) {
        if (now - messageSince < ERROR_HOLD_MS) return;
        messageHeld = false;
    }
    if (now - lastRefresh < OLED_REFRESH_INTERVAL_MS) return;
    lastRefresh = now;

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
    display.println(mqttConnected ? "CONNECTED" : "OFFLINE");

    display.setCursor(x, OLED_BODY_Y + 2 * OLED_LINE_HEIGHT + y);
    display.print("Tarif: ");
    display.println(hdoTariffText());

    display.setCursor(x, OLED_BODY_Y + 3 * OLED_LINE_HEIGHT + y);
    if (WiFi.status() == WL_CONNECTED) {
        display.print(WiFi.localIP());
    } else {
        display.print("IP: unavailable");
    }

    display.display();
}

#else

void displaySetup() {}
void displayMessage(const char*, const char*) {}
void displayHoldMessage(const char*, const char*) {}
void displayRefresh(bool) {}

#endif
