// =====================================================================
//  LED-Decke (7× ESP32, einer pro Reihe) – Konfiguration
//
//  Alle 7 ESP32 bekommen DIESELBE Firmware. Vor jedem Hochladen nur
//  ROW_NUMBER auf die Reihe des jeweiligen ESP32 stellen (1 bis 7).
// =====================================================================
#pragma once

// >>> Welche Reihe steuert dieser ESP32? (1 = im Handy-Raster ganz links)
#define ROW_NUMBER 1

// Welcher ESP32 ist der Chef? Er verbindet sich per Bluetooth mit dem Handy,
// berechnet die Effekte und schickt die Farben per Funk (ESP-NOW) an die
// anderen 6. Die anderen brauchen kein Bluetooth.
#define MASTER_ROW 1

// --- Bluetooth -------------------------------------------------------
#define BLE_NAME "LED-Decke"

// --- WLAN für Updates (OTA) -----------------------------------------
// Mit WLAN kannst du neue Firmware ohne Kabel aufspielen: In der Arduino IDE
// unter Werkzeuge -> Port erscheint dann "led-decke-reihe1" usw.
// Alle 7 ESP32 müssen dasselbe WLAN benutzen (sie funken auf dessen Kanal).
#define WIFI_SSID      ""
#define WIFI_PASSWORD  ""
#define OTA_HOSTNAME_PREFIX "led-decke-reihe"   // + Reihen-Nummer
#define OTA_PASSWORD   ""                       // leer = ohne Passwort

// Port, auf dem der Chef Befehle vom Raspberry Pi (Launchpad) annimmt
#define UDP_PORT 4210

// --- Funk zwischen den ESP32 (ESP-NOW) -------------------------------
// Nur benutzt, wenn KEIN WLAN eingetragen ist oder es nicht erreichbar ist.
// Alle 7 müssen dann auf demselben Kanal sein (1–13).
#define ESPNOW_CHANNEL 1

// --- Raster ------------------------------------------------------------
// 7 Reihen nebeneinander, jede Reihe ist 9 Kästen lang.
// Im Handy-Raster: 7 Spalten (= Reihen) und 9 Zeilen (= Kästen).
#define COLS 7
#define ROWS 9

// --- Streifen dieser Reihe ----------------------------------------------
// Jeder der 2 Streifen hat seine eigene Datenleitung:
// ESP32-Pin -> 74AHCT125 -> 330 Ω -> grünes Datenkabel (DIN) des Streifens
#define STRIP_A_PIN 16
#define STRIP_B_PIN 17

// Wie viele LEDs hat EIN Streifen? (von Louis gezählt: 83)
// Die LEDs werden gleichmäßig auf die 9 Kästen verteilt (9 oder 10 pro
// Kasten), damit keine LED dunkel bleibt.
#define LEDS_PER_STRIP 83

// Wo beginnt der Streifen (Seite mit DIN)? true = oben (Zeile 1 im Handy-
// Raster), false = unten. Ein Wert pro Reihe 1..7.
// Im Verkabelungstest läuft das Licht in jeder Reihe von oben nach unten.
// Läuft ein Streifen verkehrt herum, hier seinen Wert in dieser Reihe umdrehen.
#define STRIP_A_STARTS_AT_TOP {true, true, true, true, true, true, true}
#define STRIP_B_STARTS_AT_TOP {true, true, true, true, true, true, true}

// --- LED-Hardware ------------------------------------------------------
// RGBW-Streifen (SK6812 RGBW): NEO_GRBW. Farben vertauscht? NEO_RGBW probieren.
// Reine RGB-Streifen (WS2812B): NEO_GRB und USE_WHITE_CHANNEL false.
#define LED_PIXEL_TYPE NEO_GRBW

// true = Weiß-Anteile einer Farbe über die weiße LED ausgeben
#define USE_WHITE_CHANNEL true

// Höchste erlaubte Helligkeit (0–255). Der Regler in der App geht bis hier.
#define MAX_BRIGHTNESS 160

// Bilder pro Sekunde, die der Chef an alle schickt.
#define FPS 30

// Anzahl speicherbarer Szenen
#define MAX_SCENES 10
