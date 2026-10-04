# HDO monitor - ESP32-C3 + OLED

## Zapojení
- HDO přes modul M447B (indikátor 230 V AC s optočlenem PC817): GPIO1, aktivní LOW
  - HDO přijímač v elektroměru spíná nulový vodič; při nízkém tarifu je na modulu 230 V.
  - Modul napětí usměrní a vyhladí, výstup PC817 je tedy trvalá úroveň (LOW = nízký tarif), ne pulzy 50 Hz.
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
- `hdo/state`: JSON se stavem a diagnostikou, například
  ```json
  {"hdo":true,"rssi":-62,"uptime":86400,"ip":"192.168.1.50","reset":"power on"}
  ```
  `uptime` je v sekundách, `reset` je důvod posledního restartu (např. `brownout`,
  `task watchdog`). Klíč `hdo` zůstal, nové klíče starší konzumenty neruší.

Stav se publikuje při změně HDO, po připojení k brokeru a každých 60 s
(`REPORT_INTERVAL_MS`); všechny zprávy jsou retained.

### openHAB
Potřebné doplňky: **MQTT Binding** a **JSONPath Transformation**.
Příklad textové konfigurace (broker uprav podle sebe; pokud už bridge
na broker máš, přidej jen `Thing topic hdo` do něj):

`things/hdo.things`
```
Bridge mqtt:broker:mosquitto "Mosquitto" [ host="raspi4server.local", port=1883 ] {
    Thing topic hdo "HDO monitor" [ availabilityTopic="hdo/availability", payloadAvailable="ONLINE", payloadNotAvailable="OFFLINE" ] {
        Channels:
            Type switch : hdo    "HDO"          [ stateTopic="hdo/status", on="ON", off="OFF" ]
            Type number : rssi   "Wi-Fi signal" [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.rssi", unit="dBm" ]
            Type number : uptime "Uptime"       [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.uptime", unit="s" ]
            Type string : reset  "Reset reason" [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.reset" ]
            Type string : ip     "IP address"   [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.ip" ]
    }
}
```

`items/hdo.items`
```
Switch      HDO_Active      "HDO [%s]"                { channel="mqtt:topic:mosquitto:hdo:hdo" }
Number      HDO_RSSI        "Wi-Fi signal [%d dBm]"   { channel="mqtt:topic:mosquitto:hdo:rssi" }
Number:Time HDO_Uptime      "Uptime [%.0f s]"         { channel="mqtt:topic:mosquitto:hdo:uptime" }
String      HDO_ResetReason "Reset reason [%s]"       { channel="mqtt:topic:mosquitto:hdo:reset" }
String      HDO_IP          "IP address [%s]"         { channel="mqtt:topic:mosquitto:hdo:ip" }
```

- Kanál `hdo` nemá `commandTopic`, je tedy jen pro čtení.
- Díky `availabilityTopic` přejde Thing do stavu OFFLINE, když zařízení
  vypadne (LWT) nebo se restartuje kvůli OTA.
- Upozornění/pravidla lze stavět např. na změně `HDO_ResetReason` na
  `brownout` nebo `task watchdog`.

### Home Assistant (volitelné)
Ve výchozím stavu vypnuto. Po nastavení `HA_DISCOVERY = true` v `config.h`
zařízení po každém připojení k brokeru publikuje (retained) MQTT discovery
konfiguraci do `homeassistant/.../config` a v HA se samo objeví zařízení
**HDO monitor** s entitami:
- HDO (binary sensor, `hdo/status`)
- diagnostika: Wi-Fi signal, Uptime, Reset reason, IP address (z `hdo/state`)

Dostupnost entit řídí `hdo/availability`. Verze firmwaru (čas sestavení) je
v informacích o zařízení, takže je vidět, zda OTA prošla.

V HA musí být zapnutá MQTT integrace s discovery (výchozí stav). Pokud máš
HDO senzor nadefinovaný ručně v `configuration.yaml`, ruční konfiguraci
odstraň - jinak bude v HA dvakrát. Odebrání zařízení z HA po vypnutí discovery: smaž retained zprávy
`homeassistant/+/hdo_c3_xxxxxx/+/config` (např. MQTT Explorerem).

## Poznámky
- Wi-Fi se zkouší připojit opakovaně, bez nekonečné blokující smyčky.
- HDO se čte pollingem v každém průchodu smyčky s neblokujícím debounce (50 ms).
- OLED se obnovuje každých 500 ms; chybová hláška OTA na něm zůstane 10 s (`OLED_MESSAGE_HOLD_MS`).
- Ochrana OLED proti vypálení: snížený jas (`OLED_CONTRAST`) a posun obsahu o pár pixelů každých 5 min (`OLED_SHIFT_INTERVAL_MS`).
- Watchdog: když se hlavní smyčka zasekne na déle než 30 s (`WATCHDOG_TIMEOUT_S`), zařízení se restartuje; v logu je pak důvod resetu `task watchdog`.
- Přihlašovací údaje Wi-Fi/MQTT jsou v `src/secrets.h`, který se necommituje.
- MQTT host končící na `.local` se překládá přes mDNS; pevná IP adresa je spolehlivější.
- MQTT connect knihovny PubSubClient je synchronní krátký síťový pokus (TCP timeout 2 s); opakování je časované z hlavní smyčky s exponenciálním odstupem 5 s → 60 s, aby nedostupný broker neblokoval smyčku (a tím ani OTA). Adresa `.local` se znovu překládá až po 3 neúspěších v řadě.
