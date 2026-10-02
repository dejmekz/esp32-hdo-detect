# HDO monitor - ESP32-C3 + OLED

## Zapojení
- HDO přes PC817C: GPIO1, aktivní LOW
- OLED SSD1306 I2C: SDA GPIO8, SCL GPIO10
- OLED adresa: 0x3C (pokud displej používá 0x3D, změň `OLED_I2C_ADDRESS` v `src/config.h`)
- DS18B20 zatím není implementován

U HDO vstupu použij externí pull-up na 3,3 V (např. 10 kΩ), pokud jej již nezajišťuje tvoje zapojení PC817C.

## Build
1. Zkopíruj `src/secrets.example.h` na `src/secrets.h` a vyplň Wi-Fi/MQTT údaje (`secrets.h` je v `.gitignore`).
2. Zkopíruj `ota.secret.ini.example` na `ota.secret.ini` a nastav stejné heslo jako `OTA_PASSWORD` v `secrets.h`.
3. Otevři adresář projektu ve VS Code s PlatformIO a proveď Build/Upload (první nahrání přes USB).

## OTA (aktualizace přes Wi-Fi)
- Zařízení je dostupné jako `hdo-monitor.local` (hostname v `DEVICE_HOSTNAME`).
- Upload: prostředí `ota`, tj. `pio run -e ota -t upload`.
- Pokud Windows nepřeloží `.local`, dej do `upload_port` v `platformio.ini` IP adresu z OLED.
- Během OTA se sníží vysílací výkon Wi-Fi (`OTA_WIFI_TX_POWER`), protože při plném výkonu docházelo k brownout resetu. Pomáhá i kondenzátor na 3,3 V.

## Sériový log
- Výstup jde přes nativní USB (115200), řádky mají čas od startu v ms.
- Po startu se vypíše důvod resetu (např. `brownout`, `panic`) - užitečné při diagnostice.
- Otevření portu běžným terminálem může desku resetovat; vypnutí logu: `SERIAL_LOGGING = false` v `config.h`.

## MQTT
Zachována původní témata:
- `hdo/availability`: `ONLINE` / LWT `OFFLINE`
- `hdo/status`: `ON` / `OFF`
- `hdo/temperature`: `unknown`
- `hdo/state`: JSON, například `{"hdo":true,"temperature":null}`

## Poznámky
- Wi-Fi se zkouší připojit opakovaně, bez nekonečné blokující smyčky.
- HDO se čte pollingem v každém průchodu smyčky s neblokujícím debounce (50 ms).
- OLED se obnovuje každých 500 ms.
- Přihlašovací údaje Wi-Fi/MQTT jsou v `src/secrets.h`, který se necommituje.
- MQTT host končící na `.local` se překládá přes mDNS; pevná IP adresa je spolehlivější.
- MQTT connect knihovny PubSubClient je synchronní krátký síťový pokus; opakování je časované z hlavní smyčky.
