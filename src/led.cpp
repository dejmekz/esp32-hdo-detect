#include "led.h"

#include <Arduino.h>

#include "config.h"

static LedColor currentColor = LedColor::Off;
static bool colorSet = false;
static bool errorHeld = false;
static unsigned long errorSince = 0;

static void writeChannel(uint8_t channel, bool on, uint8_t brightness)
{
    const uint8_t duty = on ? brightness : 0;
    // Společná anoda: svítí při LOW => obrácená střída.
    // ledcWrite(255) při 8 bitech dává trvale HIGH.
    ledcWrite(channel, LED_COMMON_ANODE ? 255 - duty : duty);
}

void ledSet(LedColor color)
{
    if (colorSet && color == currentColor) return;
    currentColor = color;
    colorSet = true;

    const LedChannels channels = ledChannels(color);
    writeChannel(LED_RED_CHANNEL, channels.red, LED_RED_BRIGHTNESS);
    writeChannel(LED_GREEN_CHANNEL, channels.green, LED_GREEN_BRIGHTNESS);
    writeChannel(LED_BLUE_CHANNEL, channels.blue, LED_BLUE_BRIGHTNESS);
}

void ledSetup()
{
    ledcSetup(LED_RED_CHANNEL, LED_PWM_FREQ_HZ, LED_PWM_RESOLUTION);
    ledcSetup(LED_GREEN_CHANNEL, LED_PWM_FREQ_HZ, LED_PWM_RESOLUTION);
    ledcSetup(LED_BLUE_CHANNEL, LED_PWM_FREQ_HZ, LED_PWM_RESOLUTION);
    ledcAttachPin(LED_RED_PIN, LED_RED_CHANNEL);
    ledcAttachPin(LED_GREEN_PIN, LED_GREEN_CHANNEL);
    ledcAttachPin(LED_BLUE_PIN, LED_BLUE_CHANNEL);
    // Modrá = zařízení startuje (do prvního průchodu smyčkou).
    ledSet(LedColor::Blue);
}

void ledHoldError()
{
    errorHeld = true;
    errorSince = millis();
}

void ledRefresh(bool lowTariff, bool wifiConnected, bool mqttConnected)
{
    const unsigned long now = millis();

    if (errorHeld) {
        if (now - errorSince < ERROR_HOLD_MS) {
            ledSet(ledBlink(LedColor::Magenta, now, LED_BLINK_FAST_MS));
            return;
        }
        errorHeld = false;
    }

    ledSet(ledStatusColor(lowTariff, wifiConnected, mqttConnected, now,
                          LED_BLINK_FAST_MS, LED_BLINK_SLOW_MS));
}
