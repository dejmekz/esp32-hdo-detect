# HDO monitor - ESP32-C3 + OLED

## Zapojení
- HDO přes PC817C: GPIO1, aktivní LOW
- OLED SSD1306 I2C: SDA GPIO8, SCL GPIO10
- OLED adresa: 0x3C (pokud displej používá 0x3D, změň `OLED_I2C_ADDRESS` v `src/config.h`)

U HDO vstupu použij externí pull-up na 3,3 V (např. 10 kΩ), pokud jej již nezajišťuje tvoje zapojení PC817C.

## Build
1. Zkopíruj `src/secrets.example.h` na `src/secrets.h` a vyplň Wi-Fi/MQTT údaje (`secrets.h` je v `.gitignore`).
2. Zkopíruj `ota.secret.ini.example` na `ota.secret.ini` a nastav stejné heslo jako `OTA_PASSWORD` v `secrets.h`.
3. Otevři adresář projektu ve VS Code s PlatformIO a proveď Build/Upload (první nahrání přes USB).

## OTA (aktualizace přes Wi-Fi)
Po prvním nahrání přes USB lze další verze nahrávat bezdrátově – zařízení
může zůstat namontované v rozvaděči.

### Jednorázové nastavení
1. V `src/secrets.h` nastav `OTA_PASSWORD` (prázdné heslo = OTA bez ochrany, nedoporučeno).
2. Zkopíruj `ota.secret.ini.example` na `ota.secret.ini` a vyplň **stejné** heslo:
   ```ini
   [ota]
   password = tvoje-ota-heslo
   ```
   Soubor je v `.gitignore`. Bez něj se projekt sestaví, jen OTA upload selže na autentizaci.
3. Nahraj firmware **přes USB** (výchozí prostředí `esp32-c3-devkitm-1`). Teprve tato verze obsahuje OTA.

### Nahrání nové verze
- Příkazová řádka: `pio run -e ota -t upload`
- VS Code: ve stavové liště PlatformIO přepni prostředí na `env:ota` a klikni na Upload.
  Pro build a upload přes USB přepni zpět na `env:esp32-c3-devkitm-1`.

Zařízení se hlásí jako `hdo-monitor.local` (`DEVICE_HOSTNAME` v `config.h`).
Průběh je vidět na OLED (`OTA UPDATE` + procenta) a v sériovém logu. Před
restartem pošle do `hdo/availability` hodnotu `OFFLINE`, po startu (~5 s) zase `ONLINE`.

### Jak to funguje
- PC pošle pozvánku na UDP port 3232, ověří heslo a zařízení si pak firmware
  stáhne přes TCP spojení zpět na PC.
- Firmware se zapisuje do druhého OTA oddílu; při chybě (přerušení, špatné heslo)
  zůstává běžet původní verze.
- Během přenosu se sníží vysílací výkon Wi-Fi na 11 dBm (`OTA_WIFI_TX_POWER`).
  Při plném výkonu souběh vysílání a zápisu do flash způsoboval brownout reset;
  pomáhá i kondenzátor na napájení 3,3 V.
- Watchdog se během přenosu průběžně resetuje, upload ho nespustí.

### Řešení problémů
| Chyba | Příčina / řešení |
|---|---|
| `Host hdo-monitor.local Not Found` | Windows nepřeložil mDNS jméno, nebo zařízení zrovna startuje. Zkus znovu, případně dej do `upload_port` v `platformio.ini` IP adresu z OLED. |
| `No response from device` / `No Answer to our Authentication` | Zařízení není v síti nebo neběží jeho smyčka. Zkontroluj OLED / `hdo/availability`; v krajním případě nahraj přes USB. |
| `Authentication Failed` | Heslo v `ota.secret.ini` neodpovídá `OTA_PASSWORD` ve firmwaru, který v zařízení běží. |
| Upload se zasekne na pár procentech | Zařízení se během přenosu restartovalo – důvod resetu (např. `brownout`) ukáže sériový log po startu. |
| Upload začne, ale hned skončí chybou | Firewall Windows blokuje příchozí TCP spojení od zařízení pro `python.exe` z PlatformIO – povol ho v privátní síti. |

## Sériový log
- Výstup jde přes nativní USB (115200), řádky mají čas od startu v ms.
- Po startu se vypíše důvod resetu (např. `brownout`, `panic`) - užitečné při diagnostice.
- Otevření portu běžným terminálem může desku resetovat; vypnutí logu: `SERIAL_LOGGING = false` v `config.h`.

## MQTT
Zachována původní témata:
- `hdo/availability`: `ONLINE` / LWT `OFFLINE`
- `hdo/status`: `ON` / `OFF`
- `hdo/state`: JSON, například `{"hdo":true}`

## Poznámky
- Wi-Fi se zkouší připojit opakovaně, bez nekonečné blokující smyčky.
- HDO se čte pollingem v každém průchodu smyčky s neblokujícím debounce (50 ms).
- OLED se obnovuje každých 500 ms.
- Watchdog: když se hlavní smyčka zasekne na déle než 30 s (`WATCHDOG_TIMEOUT_S`), zařízení se restartuje; v logu je pak důvod resetu `task watchdog`.
- Přihlašovací údaje Wi-Fi/MQTT jsou v `src/secrets.h`, který se necommituje.
- MQTT host končící na `.local` se překládá přes mDNS; pevná IP adresa je spolehlivější.
- MQTT connect knihovny PubSubClient je synchronní krátký síťový pokus; opakování je časované z hlavní smyčky.
