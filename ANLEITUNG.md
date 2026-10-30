# LED-Decke mit 7 ESP32 – Steuerung per Bluetooth

## So funktioniert es

- In jeder Reihe sitzt ein ESP32 und steuert die 2 Streifen dieser Reihe.
- **Einer** davon ist der Chef (Standard: Reihe 1). Mit ihm verbindet sich
  dein Handy per **Bluetooth**.
- Der Chef berechnet 30-mal pro Sekunde das Bild für die ganze Decke und
  funkt es per **ESP-NOW** (direkter Funk zwischen ESP32, ohne Router) an
  die anderen 6. Jeder zeigt davon seine eigene Reihe. Dadurch laufen alle
  Effekte automatisch synchron.
- Alle 7 bekommen **dieselbe Firmware**. Du stellst vor dem Hochladen nur
  die Reihen-Nummer ein.

## Dateien

| Datei | Zweck |
|---|---|
| `led_decke_esp32/config.h` | Einstellungen (Reihen-Nummer, Pins, LEDs pro Kasten …) |
| `led_decke_esp32/led_decke_esp32.ino` | Die Firmware |
| `steuerseite.html` | Die Handy-Seite (Bluetooth) |
| `platformio.ini` | Nur falls du PlatformIO statt Arduino IDE nimmst |

## Anschluss pro Reihe

```
 Netzteil 5V ──┬──────────────► 5V der beiden Streifen
               ├──────────────► ESP32 VIN
               └──────────────► 74AHCT125 Pin 14 (VCC)
 Netzteil GND ─┬──────────────► GND der beiden Streifen (weißes Kabel)
               ├──────────────► ESP32 GND
               └──────────────► 74AHCT125 Pin 7 (GND)

 ESP32 GPIO16 ──► Pin 2 (1A)   Pin 3 (1Y) ──[330 Ω]──► Streifen A, grünes Kabel (DIN)
 ESP32 GPIO17 ──► Pin 5 (2A)   Pin 6 (2Y) ──[330 Ω]──► Streifen B, grünes Kabel (DIN)
 74AHCT125 Pin 1 (1OE) und Pin 4 (2OE) ──► GND   (schaltet die beiden Ausgänge ein)
 Unbenutzt: Pin 10 und 13 ──► 5V,  Pin 9 und 12 ──► GND
```

- Das grüne Kabel muss am **Anfang** des Streifens sitzen (DIN, der Pfeil auf
  dem Streifen zeigt vom Kabel weg).
- Alle GND müssen verbunden sein, sonst flackert es.
- Zum Hochladen per USB am sichersten das Netzteil ausschalten.

## Firmware hochladen (Arduino IDE)

1. Arduino IDE installieren. Unter *Datei → Einstellungen → Zusätzliche
   Boardverwalter-URLs* eintragen:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. *Werkzeuge → Board → Boardverwalter*: **esp32** von Espressif installieren.
3. *Sketch → Bibliothek einbinden → Bibliotheken verwalten*:
   **Adafruit NeoPixel**, **ArduinoJson** und **NimBLE-Arduino** installieren.
4. `led_decke_esp32/led_decke_esp32.ino` öffnen.
5. *Werkzeuge*: Board **ESP32 Dev Module**, Partition Scheme
   **Minimal SPIFFS (1.9MB APP with OTA)**, richtigen Port wählen.
6. In `config.h` dein WLAN eintragen (`WIFI_SSID`, `WIFI_PASSWORD`).
   Das brauchst du für Updates ohne Kabel, siehe unten.
7. Für jeden der 7 ESP32:
   - In `config.h` `#define ROW_NUMBER` auf seine Reihe stellen (1 bis 7,
     Reihe 1 ist im Handy-Raster ganz links).
   - Hochladen (Pfeil-Knopf).
   - Am besten den ESP32 gleich mit der Nummer beschriften.

Im *Seriellen Monitor* (115200 Baud) meldet sich jeder ESP32 mit seiner Reihe.

## Updates ohne Kabel (über WLAN)

Reihe 1 hängt schon mit dem Testcode an der Decke, der ebenfalls Updates über
WLAN kann. Deshalb kannst du diese Firmware direkt über WLAN aufspielen
(Tools → Port → Netzwerk-Port der Reihe 1), ohne sie abzunehmen.

Wenn das WLAN in `config.h` steht, kannst du später neue Firmware aufspielen,
ohne die ESP32 abzunehmen:

1. Rechner im selben WLAN, Arduino IDE offen.
2. *Werkzeuge → Port*: dort stehen unter „Netzwerk-Ports“
   **led-decke-reihe1** bis **led-decke-reihe7**.
3. Port wählen, `ROW_NUMBER` passend einstellen, hochladen.

Wichtig: Alle 7 müssen im **selben WLAN** sein, denn sie funken auf dessen
Kanal miteinander. Ist das WLAN aus, schalten sie auf `ESPNOW_CHANNEL` um und
die Decke läuft trotzdem weiter (nur Updates gehen dann nicht).
Wenn du `OTA_PASSWORD` setzt, fragt die Arduino IDE beim Hochladen danach.

## Steuerseite am Handy öffnen

Web-Bluetooth funktioniert nur, wenn die Seite über **https** geladen wird.
Am einfachsten liegt `steuerseite.html` dafür auf GitHub Pages (kostenlos).
Das richte ich dir gern ein, du musst dafür nur ein leeres GitHub-Repository
anlegen.

- **Android:** Seite in **Chrome** öffnen.
- **iPhone/iPad:** Safari kann kein Bluetooth. Lade die kostenlose App
  **„Bluefy – Web BLE Browser“** aus dem App Store und öffne die Seite darin.

Dann auf **„Mit Bluetooth verbinden“** tippen und **LED-Decke** auswählen.
Tipp: Seite zum Home-Bildschirm hinzufügen.

## Einrichten mit dem Verkabelungstest

Wähle in der App **Verkabelungstest**. Das Licht läuft Reihe für Reihe von
oben nach unten, der oberste Kasten jeder Reihe leuchtet rot.

- Leuchtet die falsche Reihe? → `ROW_NUMBER` dieses ESP32 stimmt nicht.
- Läuft ein Streifen von unten nach oben? → in `config.h` bei
  `STRIP_A_STARTS_AT_TOP` bzw. `STRIP_B_STARTS_AT_TOP` den Wert dieser Reihe
  auf `false` stellen und neu hochladen. Am besten gleich für alle 7 gleich
  einstellen, dann bleibt nur `ROW_NUMBER` unterschiedlich.
- Licht sitzt „zwischen“ zwei Kästen? → `LEDS_PER_STRIP` prüfen (Standard 83,
  die LEDs werden gleichmäßig auf die 9 Kästen verteilt).
- Farben vertauscht? → `LED_PIXEL_TYPE NEO_RGBW` probieren (Standard für
  deine RGBW-Streifen ist `NEO_GRBW`).
- Weiß wird über die eigene weiße LED ausgegeben (`USE_WHITE_CHANNEL true`).

## Probleme

- **Eine Reihe bleibt dunkel:** Läuft der ESP32 (Serieller Monitor)? Gleicher
  `ESPNOW_CHANNEL` wie der Chef? Die Reihen-ESP32 brauchen den Chef: ist er
  aus, behalten sie ihr letztes Bild.
- **Handy findet die Decke nicht:** Nur der Chef hat Bluetooth. Bluetooth und
  Standort am Handy an? Ist schon ein anderes Handy verbunden?
- **Ruckeln/Aussetzer:** Den Chef möglichst in die Mitte der Decke (Reihe 4)
  setzen. Dazu `MASTER_ROW 4` bei **allen** 7 einstellen.
