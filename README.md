# HDO monitor - ESP32-C3 + RGB LED

## Zapojení
- HDO přes modul M447B (indikátor 230 V AC s optočlenem PC817): GPIO1, aktivní LOW
  - HDO přijímač v elektroměru spíná nulový vodič; při nízkém tarifu je na modulu 230 V.
  - Modul napětí usměrní a vyhladí, výstup PC817 je tedy trvalá úroveň (LOW = nízký tarif), ne pulzy 50 Hz.
- RGB LED se společnou katodou (katoda na GND): modrá GPIO3, červená GPIO4, zelená GPIO5
  - každá barva přes vlastní předřadný rezistor; jas se dolaďuje PWM (`LED_RED_BRIGHTNESS`, `LED_GREEN_BRIGHTNESS`, `LED_BLUE_BRIGHTNESS`)
  - LED se společnou anodou (na 3,3 V): nastav `LED_COMMON_ANODE = true` v `src/config.h`
- I2C sběrnice (μŠup konektor): SDA GPIO8, SCL GPIO10 (`I2C_SDA_PIN`, `I2C_SCL_PIN`), sdílí ji senzor i OLED
- Senzor teploty a vlhkosti Grove DHT20 (Temperature & Humidity Sensor V2.0, volitelný): I2C, adresa 0x38, napájení 3,3 V
  - detekuje se při startu a pak každou minutu (`SENSOR_PROBE_INTERVAL_MS`), lze ho tedy připojit i za běhu;
    po 3 chybách měření v řadě se považuje za odpojený a hledá se znovu
  - když stavový bajt nehlásí kalibraci, inicializují se registry 0x1B, 0x1C a 0x1E podle vzorového kódu výrobce
  - bez senzoru zařízení běží dál, teplota a vlhkost jsou v `hdo/state` jako `null`
  - senzor umísti mimo desku ESP32 a zdroj - jejich ohřev zkresluje měření
- OLED SSD1306 I2C (volitelný, ve výchozím buildu vypnutý): adresa 0x3C
  - build s OLED: prostředí `esp32-c3-oled` (`pio run -e esp32-c3-oled -t upload`); pro OTA upload
    s OLED změň v `platformio.ini` u `env:esp32-c3-devkitm-1` flag na `-DHDO_OLED=1`
  - při adrese 0x3D změň `OLED_I2C_ADDRESS`

### Význam LED
| LED | Stav |
|---|---|
| zelená trvale | nízký tarif (NT), Wi-Fi i MQTT připojené |
| červená trvale | vysoký tarif (VT), Wi-Fi i MQTT připojené |
| barva tarifu bliká pomalu | Wi-Fi OK, MQTT nepřipojené |
| barva tarifu bliká rychle | bez Wi-Fi |
| modrá trvale | start zařízení (krátce), konec OTA |
| modrá problikává | probíhá OTA |
| fialová rychle bliká 10 s | chyba OTA |

U HDO vstupu použij externí pull-up na 3,3 V (např. 10 kΩ), pokud jej již nezajišťuje tvoje zapojení PC817C.

## Build
1. Zkopíruj `src/secrets.example.h` na `src/secrets.h` a vyplň Wi-Fi/MQTT údaje (`secrets.h` je v `.gitignore`).
2. Zkopíruj `ota.secret.ini.example` na `ota.secret.ini` a nastav stejné heslo jako `OTA_PASSWORD` v `secrets.h`.
3. Otevři adresář projektu ve VS Code s PlatformIO a proveď Build/Upload (první nahrání přes USB).

### Struktura kódu
| Soubor | Obsah |
|---|---|
| `src/main.cpp` | `setup()` / `loop()`, sériový log, watchdog, důvod resetu |
| `src/hdo.*` | vstup HDO s debounce |
| `src/sensor.*` | DHT20: měření na pozadí, hledání za běhu |
| `src/led.*` | stavová RGB LED |
| `src/display.*` | volitelný OLED (`HDO_OLED`) |
| `src/network.*` | Wi-Fi, MQTT, publikování, HA discovery |
| `src/ota.*` | OTA přes Wi-Fi |
| `src/config.h` | nastavení (piny, časy, MQTT) |
| `lib/HdoCore/` | čistá logika bez Arduino API (debounce, odstup, DHT20, LED, JSON) - pokrytá unit testy |

### Testy a CI
- Unit testy logiky z `lib/HdoCore` (`test/test_core`): `pio test -e native`.
  Potřebují `gcc`/`g++` v PATH (na Windows např. MSYS2/MinGW), jinak běží v CI.
- GitHub Actions (`.github/workflows/build.yml`) při každém push spustí testy a sestaví
  firmware s LED i s OLED; `secrets.h` se v CI vytvoří ze šablony.

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
Průběh je vidět na LED (modře problikává; s OLED `OTA UPDATE` + procenta) a v sériovém logu. Před
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
| `Host hdo-monitor.local Not Found` | Windows nepřeložil mDNS jméno, nebo zařízení zrovna startuje. Zkus znovu, případně dej do `upload_port` v `platformio.ini` IP adresu ze sériového logu / `hdo/state`. |
| `No response from device` / `No Answer to our Authentication` | Zařízení není v síti nebo neběží jeho smyčka. Zkontroluj LED / `hdo/availability`; v krajním případě nahraj přes USB. |
| `Authentication Failed` | Heslo v `ota.secret.ini` neodpovídá `OTA_PASSWORD` ve firmwaru, který v zařízení běží. |
| Upload se zasekne na pár procentech | Zařízení se během přenosu restartovalo – důvod resetu (např. `brownout`) ukáže sériový log po startu. |
| Upload začne, ale hned skončí chybou | Firewall Windows blokuje příchozí TCP spojení od zařízení pro `python.exe` z PlatformIO – povol ho v privátní síti. |

## Sériový log
- Výstup jde přes nativní USB (115200), řádky mají čas od startu v ms.
- Po startu se vypíše důvod resetu (např. `brownout`, `panic`) - užitečné při diagnostice.
- Opakované restarty s důvodem `brownout` = napájení nezvládá proudové špičky
  při vysílání Wi-Fi. Vysílací výkon je proto i za běžného provozu snížen
  na 11 dBm (`WIFI_TX_POWER` v `config.h`); pomůže i kondenzátor 470–1000 µF
  na 5 V u desky a krátký, kvalitní napájecí kabel.
- Otevření portu běžným terminálem může desku resetovat; vypnutí logu: `SERIAL_LOGGING = false` v `config.h`.

## MQTT
Zachována původní témata:
- `hdo/availability`: `ONLINE` / LWT `OFFLINE`
- `hdo/status`: `ON` = nízký tarif (NT), `OFF` = vysoký tarif (VT); log (a OLED) ukazují přímo `NT` / `VT`
- `hdo/state`: JSON se stavem a diagnostikou, například
  ```json
  {"hdo":true,"rssi":-62,"uptime":86400,"ip":"192.168.1.50","reset":"power on","fw":"3cc3749","temperature":21.4,"humidity":48.2}
  ```
  `uptime` je v sekundách, `reset` je důvod posledního restartu (např. `brownout`,
  `task watchdog`), `fw` je verze firmwaru z `git describe` (`-dirty` = sestaveno
  z necommitnutých změn). `temperature` (°C) a `humidity` (%) z DHT20 mají hodnotu
  jen tehdy, když je senzor připojený a poslední měření není starší než 30 s (měří se
  každých 10 s, `SENSOR_READ_INTERVAL_MS`); jinak jsou `null`. Klíč `hdo` zůstal,
  nové klíče starší konzumenty neruší.

Stav se publikuje při změně HDO, po připojení k brokeru a každých 60 s
(`REPORT_INTERVAL_MS`); všechny zprávy jsou retained.

### openHAB
Ověřeno na openHAB 5.2. Potřebné doplňky: **MQTT Binding**,
**JSONPath Transformation** a pro sitemapu **Map Transformation**.

Konfigurace počítá s existujícím bridge na broker s UID
`mqtt:broker:mosquitto` - Thing se k němu jen připojí, stávající soubory
není třeba upravovat. Pokud má tvůj bridge jiné UID, změň ho v `hdo.things`
(v závorce) i v `hdo.items` (`mqtt:topic:<bridge>:hdo:...`). Pokud bridge
ještě nemáš, založ ho, např.:
```
Bridge mqtt:broker:mosquitto "Mosquitto" [ host="127.0.0.1", port=1883, username="...", password="..." ]
```

`things/hdo.things`
```
Thing mqtt:topic:mosquitto:hdo "HDO monitor" (mqtt:broker:mosquitto) @ "HDO"
  [ availabilityTopic="hdo/availability", payloadAvailable="ONLINE", payloadNotAvailable="OFFLINE" ]
{
  Channels:
    Type switch : hdo         "HDO (nízký tarif)"     [ stateTopic="hdo/status", on="ON", off="OFF" ]
    Type switch : reachable   "Dostupnost"            [ stateTopic="hdo/availability", on="ONLINE", off="OFFLINE" ]
    Type number : rssi        "WiFi Signal Strength"  [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.rssi", unit="dBm" ]
    Type number : uptime      "Up time"               [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.uptime", unit="s" ]
    Type string : reset       "Reset reason"          [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.reset" ]
    Type string : ip          "IP address"            [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.ip" ]
    Type string : fw          "Firmware"              [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.fw" ]
    Type number : temperature "Teplota"               [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.temperature", unit="°C" ]
    Type number : humidity    "Vlhkost"               [ stateTopic="hdo/state", transformationPattern="JSONPATH:$.humidity", unit="%" ]
}
```

`items/hdo.items` (nadřazená skupina `MainGroup` je jen příklad - uprav nebo vynech)
```
Group        gHdo             "HDO"                    <energy>            (MainGroup)

Switch       HDO_LowTariff    "Nízký tarif [%s]"       <energy>            (gHdo)  {channel="mqtt:topic:mosquitto:hdo:hdo"}
Switch       HDO_Reachable    "Dostupnost [%s]"                            (gHdo)  {channel="mqtt:topic:mosquitto:hdo:reachable"}
Number       HDO_RSSI         "RSSI [%d dBm]"          <qualityofservice>  (gHdo)  {channel="mqtt:topic:mosquitto:hdo:rssi"}
Number:Time  HDO_UpTime       "Běží [%.1f h]"          <time>              (gHdo)  {channel="mqtt:topic:mosquitto:hdo:uptime"}
String       HDO_ResetReason  "Důvod restartu [%s]"    <error>             (gHdo)  {channel="mqtt:topic:mosquitto:hdo:reset"}
String       HDO_IP           "IP adresa [%s]"         <network>           (gHdo)  {channel="mqtt:topic:mosquitto:hdo:ip"}
String       HDO_Firmware     "Firmware [%s]"          <settings>          (gHdo)  {channel="mqtt:topic:mosquitto:hdo:fw"}
Number:Temperature   HDO_Temperature "Teplota [%.1f °C]"  <temperature>  (gHdo)  {channel="mqtt:topic:mosquitto:hdo:temperature", expire="3m"}
Number:Dimensionless HDO_Humidity    "Vlhkost [%.0f %%]"  <humidity>     (gHdo)  {channel="mqtt:topic:mosquitto:hdo:humidity", unit="%", expire="3m"}
```

`transform/hdo.map`
```
ON=NT (nízký)
OFF=VT (vysoký)
NULL=Neznámo
UNDEF=Neznámo
```

Rámec do sitemapy (např. `sitemaps/default.sitemap`); `switch2online.map`
převádí ON/OFF na ONLINE/OFFLINE - pokud ho nemáš, vynech `MAP(...)`:
```
  Frame label="HDO"
  {
    Text item=HDO_LowTariff label="Tarif [MAP(hdo.map):%s]" icon="energy" valuecolor=[ON="green", OFF="orange"] {
      Text item=HDO_Reachable label="Dostupnost [MAP(switch2online.map):%s]" valuecolor=[ON="green", OFF="red"]
      Text item=HDO_RSSI
      Text item=HDO_UpTime
      Text item=HDO_ResetReason valuecolor=[=="brownout"="red", =="task watchdog"="red", =="panic"="red"]
      Text item=HDO_IP
      Text item=HDO_Firmware
      Text item=HDO_Temperature
      Text item=HDO_Humidity
    }
  }
```

- Kanály nemají `commandTopic`, jsou jen pro čtení; v sitemapě jsou proto
  jako `Text`, ne `Switch` (přepnutí v UI by jen změnilo stav položky).
- Díky `availabilityTopic` přejde Thing do stavu OFFLINE, když zařízení
  vypadne (LWT) nebo se restartuje kvůli OTA; totéž ukazuje `HDO_Reachable`.
- `HDO_LowTariff` = ON znamená nízký tarif (NT). Na něj lze navázat pravidla,
  např. spínání ohřevu.
- Upozornění lze stavět na změně `HDO_ResetReason` na `brownout`,
  `task watchdog` nebo `panic`.
- `expire="3m"` u teploty a vlhkosti: když senzor 3 minuty neposílá
  hodnotu (odpojený, vadný, `null` v JSON), položka přejde na `UNDEF`
  místo toho, aby navždy ukazovala poslední změřenou hodnotu.

### Home Assistant (volitelné)
Ve výchozím stavu vypnuto. Po nastavení `HA_DISCOVERY = true` v `config.h`
zařízení po každém připojení k brokeru publikuje (retained) MQTT discovery
konfiguraci do `homeassistant/.../config` a v HA se samo objeví zařízení
**HDO monitor** s entitami:
- HDO (binary sensor, `hdo/status`)
- Temperature a Humidity (z `hdo/state`, jakmile je DHT20 detekován - i za běhu)
- diagnostika: Wi-Fi signal, Uptime, Reset reason, IP address (z `hdo/state`)

Dostupnost entit řídí `hdo/availability`. Verze firmwaru (git verze a čas sestavení) je
v informacích o zařízení, takže je vidět, zda OTA prošla.

V HA musí být zapnutá MQTT integrace s discovery (výchozí stav). Pokud máš
HDO senzor nadefinovaný ručně v `configuration.yaml`, ruční konfiguraci
odstraň - jinak bude v HA dvakrát. Odebrání zařízení z HA po vypnutí discovery: smaž retained zprávy
`homeassistant/+/hdo_c3_xxxxxx/+/config` (např. MQTT Explorerem).

## Poznámky
- Wi-Fi se zkouší připojit opakovaně, bez nekonečné blokující smyčky.
- Úsporný režim Wi-Fi je vypnutý (`WiFi.setSleep(false)`): zařízení je napájené ze sítě
  a bez něj spolehlivěji odpovídá na mDNS, OTA a MQTT. Průměrný odběr je o desítky mA vyšší.
- HDO se čte pollingem v každém průchodu smyčky s neblokujícím debounce (50 ms).
- LED se obnovuje v každém průchodu smyčky; při blokujícím pokusu o MQTT/mDNS (až 2 s) se blikání krátce zastaví.
- OLED (je-li zapnutý) se obnovuje každých 500 ms; chybová hláška OTA na něm (i fialové blikání LED) zůstane 10 s (`ERROR_HOLD_MS`).
- Ochrana OLED proti vypálení: snížený jas (`OLED_CONTRAST`) a posun obsahu o pár pixelů každých 5 min (`OLED_SHIFT_INTERVAL_MS`).
- Watchdog: když se hlavní smyčka zasekne na déle než 30 s (`WATCHDOG_TIMEOUT_S`), zařízení se restartuje; v logu je pak důvod resetu `task watchdog`.
- Přihlašovací údaje Wi-Fi/MQTT jsou v `src/secrets.h`, který se necommituje.
- MQTT host končící na `.local` se překládá přes mDNS; pevná IP adresa je spolehlivější.
- MQTT connect knihovny PubSubClient je synchronní krátký síťový pokus (TCP timeout 2 s); opakování je časované z hlavní smyčky s exponenciálním odstupem 5 s → 60 s, aby nedostupný broker neblokoval smyčku (a tím ani OTA). Adresa `.local` se znovu překládá až po 3 neúspěších v řadě.
