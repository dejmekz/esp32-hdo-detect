// Unit testy čisté logiky z lib/HdoCore: pio test -e native

#include <string.h>
#include <unity.h>

#include <backoff.h>
#include <debounce.h>
#include <dht20_codec.h>
#include <led_logic.h>
#include <state_json.h>

void setUp() {}
void tearDown() {}

// ---------- Debouncer ----------

void test_debounce_takes_state_after_stable_period()
{
    Debouncer d(50);
    d.reset(false, 0);

    TEST_ASSERT_FALSE(d.update(true, 100));  // hrana
    TEST_ASSERT_FALSE(d.update(true, 149));  // ještě ne 50 ms
    TEST_ASSERT_TRUE(d.update(true, 150));   // stabilní 50 ms
    TEST_ASSERT_TRUE(d.state());
    TEST_ASSERT_FALSE(d.update(true, 200));  // beze změny
}

void test_debounce_ignores_short_glitch()
{
    Debouncer d(50);
    d.reset(false, 0);

    d.update(true, 100);
    TEST_ASSERT_FALSE(d.update(false, 120));  // zákmit 20 ms
    TEST_ASSERT_FALSE(d.update(false, 500));
    TEST_ASSERT_FALSE(d.state());
}

void test_debounce_handles_millis_overflow()
{
    Debouncer d(50);
    d.reset(false, 0xFFFFFFF0);

    d.update(true, 0xFFFFFFF0);
    TEST_ASSERT_FALSE(d.update(true, 0x00000010));  // 32 ms
    TEST_ASSERT_TRUE(d.update(true, 0x00000022));   // 50 ms
}

// ---------- Backoff ----------

void test_backoff_doubles_up_to_max()
{
    Backoff b(5000, 60000);
    const uint32_t expected[] = {10000, 20000, 40000, 60000, 60000};

    TEST_ASSERT_EQUAL_UINT32(5000, b.delayMs());
    for (uint32_t delay : expected) {
        b.failed();
        TEST_ASSERT_EQUAL_UINT32(delay, b.delayMs());
    }
    TEST_ASSERT_EQUAL_UINT8(5, b.failures());
}

void test_backoff_reset()
{
    Backoff b(5000, 60000);
    b.failed();
    b.failed();
    b.reset();

    TEST_ASSERT_EQUAL_UINT32(5000, b.delayMs());
    TEST_ASSERT_EQUAL_UINT8(0, b.failures());
}

void test_backoff_failures_saturate()
{
    Backoff b(5000, 60000);
    for (int i = 0; i < 300; i++) b.failed();

    TEST_ASSERT_EQUAL_UINT8(255, b.failures());
}

// ---------- DHT20 ----------

void test_dht20_crc_check_value()
{
    // CRC-8/NRSC-5 (poly 0x31, init 0xFF): kontrolní hodnota 0xF7.
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX8(0xF7, dht20Crc(data, sizeof(data)));
}

void test_dht20_decode()
{
    // Vlhkost 0x80000 = 50 %, teplota 0x60000 = 25 °C.
    uint8_t data[7] = {0x1C, 0x80, 0x00, 0x06, 0x00, 0x00, 0x00};
    data[6] = dht20Crc(data, 6);

    Dht20Reading r = {0, 0};
    TEST_ASSERT_TRUE(dht20Decode(data, r));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, r.humidity);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, r.temperature);
}

void test_dht20_decode_rejects_bad_crc()
{
    uint8_t data[7] = {0x1C, 0x80, 0x00, 0x06, 0x00, 0x00, 0x00};
    data[6] = dht20Crc(data, 6) ^ 0x01;

    Dht20Reading r = {1.0f, 2.0f};
    TEST_ASSERT_FALSE(dht20Decode(data, r));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.temperature);  // beze změny
}

void test_dht20_status_bits()
{
    TEST_ASSERT_TRUE(dht20Busy(0x9C));
    TEST_ASSERT_FALSE(dht20Busy(0x1C));
    TEST_ASSERT_TRUE(dht20Calibrated(0x18));
    TEST_ASSERT_FALSE(dht20Calibrated(0x10));
}

// ---------- LED ----------

void test_led_steady_when_connected()
{
    TEST_ASSERT(ledStatusColor(true, true, true, 0, 150, 600) == LedColor::Green);
    TEST_ASSERT(ledStatusColor(false, true, true, 150, 150, 600) == LedColor::Red);
}

void test_led_blinks_fast_without_wifi()
{
    TEST_ASSERT(ledStatusColor(true, false, false, 0, 150, 600) == LedColor::Green);
    TEST_ASSERT(ledStatusColor(true, false, false, 150, 150, 600) == LedColor::Off);
    TEST_ASSERT(ledStatusColor(true, false, false, 300, 150, 600) == LedColor::Green);
}

void test_led_blinks_slow_without_mqtt()
{
    TEST_ASSERT(ledStatusColor(false, true, false, 150, 150, 600) == LedColor::Red);
    TEST_ASSERT(ledStatusColor(false, true, false, 600, 150, 600) == LedColor::Off);
}

void test_led_channels()
{
    const LedChannels magenta = ledChannels(LedColor::Magenta);
    TEST_ASSERT_TRUE(magenta.red);
    TEST_ASSERT_FALSE(magenta.green);
    TEST_ASSERT_TRUE(magenta.blue);

    const LedChannels off = ledChannels(LedColor::Off);
    TEST_ASSERT_FALSE(off.red || off.green || off.blue);
}

// ---------- hdo/state JSON ----------

static StateInfo sampleState()
{
    StateInfo s;
    s.lowTariff = true;
    s.rssi = -62;
    s.uptimeS = 86400;
    s.ip = "192.168.1.50";
    s.resetReason = "power on";
    s.firmware = "baeedec";
    s.freeHeap = 201234;
    s.minFreeHeap = 187654;
    s.tariffS = 3600;
    s.climateValid = true;
    s.temperature = 21.44f;
    s.humidity = 48.24f;
    return s;
}

void test_state_json_with_climate()
{
    char buf[320];
    TEST_ASSERT_TRUE(formatStateJson(buf, sizeof(buf), sampleState()));
    TEST_ASSERT_EQUAL_STRING(
        "{\"hdo\":true,\"rssi\":-62,\"uptime\":86400,\"ip\":\"192.168.1.50\","
        "\"reset\":\"power on\",\"fw\":\"baeedec\","
        "\"heap\":201234,\"heap_min\":187654,\"tariff_s\":3600,"
        "\"temperature\":21.4,\"humidity\":48.2}",
        buf);
}

void test_state_json_null_climate()
{
    StateInfo s = sampleState();
    s.lowTariff = false;
    s.climateValid = false;

    char buf[256];
    TEST_ASSERT_TRUE(formatStateJson(buf, sizeof(buf), s));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"hdo\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"temperature\":null,\"humidity\":null}"));
}

void test_state_json_buffer_too_small()
{
    char buf[32];
    TEST_ASSERT_FALSE(formatStateJson(buf, sizeof(buf), sampleState()));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_debounce_takes_state_after_stable_period);
    RUN_TEST(test_debounce_ignores_short_glitch);
    RUN_TEST(test_debounce_handles_millis_overflow);
    RUN_TEST(test_backoff_doubles_up_to_max);
    RUN_TEST(test_backoff_reset);
    RUN_TEST(test_backoff_failures_saturate);
    RUN_TEST(test_dht20_crc_check_value);
    RUN_TEST(test_dht20_decode);
    RUN_TEST(test_dht20_decode_rejects_bad_crc);
    RUN_TEST(test_dht20_status_bits);
    RUN_TEST(test_led_steady_when_connected);
    RUN_TEST(test_led_blinks_fast_without_wifi);
    RUN_TEST(test_led_blinks_slow_without_mqtt);
    RUN_TEST(test_led_channels);
    RUN_TEST(test_state_json_with_climate);
    RUN_TEST(test_state_json_null_climate);
    RUN_TEST(test_state_json_buffer_too_small);
    return UNITY_END();
}
