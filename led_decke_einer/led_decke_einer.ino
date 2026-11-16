// LED-Decke – Firmware für EINEN ESP32, der alle 14 Streifen steuert
//
// Der ESP32 verbindet sich per Bluetooth mit der Steuerseite am Handy und mit
// dem Raspberry Pi (Launchpad). Updates (OTA) gehen per WLAN.
// NEU: Solange das WLAN verbunden ist, nimmt die Decke dieselben JSON-Befehle
// auch per HTTP an (POST /api) und zeigt ihren Zustand unter GET /state.
// Erreichbar unter http://led-decke-reihe1.local oder http://<IP>.
// Einstellungen stehen direkt hier unten.

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPUpdateServer.h>
#include <Update.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

// ======================= EINSTELLUNGEN =======================
#define BLE_NAME "LED-Decke"
#define WIFI_SSID      ""   // hier dein WLAN eintragen
#define WIFI_PASSWORD  ""
#define HOSTNAME       "led-decke-reihe1"   // Name für Updates (Port in der Arduino IDE)
#define OTA_PASSWORD   ""
#define UDP_PORT 4210
#define COLS 7    // Reihen an der Decke (im Handy-Raster: Spalten)
#define ROWS 9    // Kästen pro Reihe
#define LEDS_PER_STRIP 83

// LEDs pro Kasten, vom Anfang des Streifens aus gezählt (zusammen 83)
const uint8_t BOX_LEDS[ROWS] = {8, 10, 10, 9, 9, 9, 10, 9, 9};

// Datenpins: pro Reihe 1..7 ein Pin für Streifen A und einer für Streifen B
const uint8_t PIN_A[COLS] = {16, 18, 21, 23, 26, 32, 4};
const uint8_t PIN_B[COLS] = {17, 19, 22, 25, 27, 33, 13};

// Beginnt der Streifen oben (Zeile 1 im Handy-Raster)? Ein Wert pro Reihe.
// Läuft ein Streifen im Verkabelungstest verkehrt herum: Wert umdrehen.
const bool A_STARTS_AT_TOP[COLS] = {true, true, true, true, true, true, true};
const bool B_STARTS_AT_TOP[COLS] = {true, true, true, true, true, true, true};

#define MIC_PIN 34   // Mikrofon-Modul: OUT an GPIO 34, VCC an 3V3, GND an GND

#define LED_PIXEL_TYPE NEO_GRBW
#define USE_WHITE_CHANNEL true
#define MAX_BRIGHTNESS 160
#define FPS 20
#define MAX_SCENES 10
// Bluetooth-Selbstheilung: ist 30 Minuten lang niemand verbunden, wird Bluetooth
// einmal komplett neu gestartet (ca. 1 Sekunde nicht sichtbar). 0 = nie.
#define BLE_IDLE_RESTART_MS (30UL * 60UL * 1000UL)
// Sind alle Bluetooth-Plätze belegt, aber 10 Minuten lang kam nichts mehr an
// (Geisterverbindung), werden alle Verbindungen getrennt.
#define BLE_GHOST_MS (10UL * 60UL * 1000UL)
// =============================================================

#define BLE_SERVICE_UUID "6e1d0001-8a3c-4c5b-9b1e-1ed0c0dec0de"
#define BLE_CMD_UUID     "6e1d0002-8a3c-4c5b-9b1e-1ed0c0dec0de"  // schreiben: JSON-Befehl
#define BLE_STATE_UUID   "6e1d0003-8a3c-4c5b-9b1e-1ed0c0dec0de"  // lesen: Zustand (binär)
#define BLE_SCENES_UUID  "6e1d0004-8a3c-4c5b-9b1e-1ed0c0dec0de"  // lesen: Szenen-Namen (JSON)
#define BLE_MIC_UUID     "6e1d0005-8a3c-4c5b-9b1e-1ed0c0dec0de"  // lesen: Mikrofon-Anzeige (binär)

// ---------------------------------------------------------------------
//  Farben
// ---------------------------------------------------------------------
struct CRGB {
  uint8_t r, g, b;
  CRGB() : r(0), g(0), b(0) {}
  CRGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
  explicit CRGB(uint32_t c) : r(c >> 16), g(c >> 8), b(c) {}
};
const CRGB BLACK(0, 0, 0), RED(255, 0, 0), WHITE(255, 255, 255);

// Farbkreis: h, s, v jeweils 0..255
CRGB hsv(uint8_t h, uint8_t s, uint8_t v) {
  uint8_t region = h / 43, rem = (h - region * 43) * 6;
  uint8_t p = v * (255 - s) / 255, q = v * (255 - s * rem / 255) / 255,
          t = v * (255 - s * (255 - rem) / 255) / 255;
  switch (region) {
    case 0: return CRGB(v, t, p);
    case 1: return CRGB(q, v, p);
    case 2: return CRGB(p, v, t);
    case 3: return CRGB(p, q, v);
    case 4: return CRGB(t, p, v);
    default: return CRGB(v, p, q);
  }
}

// ---------------------------------------------------------------------
//  14 Streifen
// ---------------------------------------------------------------------
const int NUM_STRIPS = COLS * 2;
Adafruit_NeoPixel *strips[NUM_STRIPS];  // 0..6 = Streifen A, 7..13 = Streifen B
uint8_t stripCol[NUM_STRIPS];
uint16_t boxFirst[NUM_STRIPS][ROWS];  // erste LED des Kastens
uint16_t boxCount[NUM_STRIPS][ROWS];  // Anzahl LEDs im Kasten

// Streifen nach BOX_LEDS auf die Kästen verteilen
void addLeds() {
  for (int k = 0; k < NUM_STRIPS; k++) {
    int col = k % COLS;
    bool isA = k < COLS;
    stripCol[k] = col;
    strips[k] = new Adafruit_NeoPixel(LEDS_PER_STRIP, isA ? PIN_A[col] : PIN_B[col], LED_PIXEL_TYPE + NEO_KHZ800);
    bool down = isA ? A_STARTS_AT_TOP[col] : B_STARTS_AT_TOP[col];
    int from = 0;
    for (int i = 0; i < ROWS; i++) {
      int row = down ? i : ROWS - 1 - i;
      boxFirst[k][row] = from;
      boxCount[k][row] = min(BOX_LEDS[i], (uint8_t)max(0, LEDS_PER_STRIP - from));
      from += BOX_LEDS[i];
    }
    strips[k]->begin();
  }
}

CRGB frame[ROWS][COLS];        // aktuelles Bild der ganzen Decke
void handleUdp();
uint8_t shownBrightness = 0;

// Der ESP32 schickt die Streifen nacheinander raus. Nach jedem Streifen wird
// der vorige Pin wieder fest auf LOW gelegt, damit seine Leitung nicht offen ist.
// Gegen Flackern: Ein Streifen bekommt nur Daten, wenn sich seine Farben geändert
// haben (dann zweimal hintereinander, damit ein gestörtes Bild sofort überschrieben
// wird). Ein stehendes Bild wird danach gar nicht mehr neu gesendet.
uint8_t lastSent[NUM_STRIPS][LEDS_PER_STRIP * 4];
uint8_t resend[NUM_STRIPS];
int lastPin = -1;

void showAll() {
  for (int k = 0; k < NUM_STRIPS; k++) {
    Adafruit_NeoPixel *s = strips[k];
    s->setBrightness(255);  // Helligkeit wird unten selbst gerechnet
    // Helligkeit wie auf der Handy-Seite: 0..1 (1 = 100 %)
    float level = (float)shownBrightness / MAX_BRIGHTNESS;
    float cap = MAX_BRIGHTNESS / 255.0f;
    for (int r = 0; r < ROWS; r++) {
      CRGB c = frame[r][stripCol[k]];
      uint8_t w = 0;
      float wOut = 0, boost = 0;
#if USE_WHITE_CHANNEL
      // Weiß-Anteil: bis 50 % Helligkeit nur die weiße LED (bei 50 % voll an),
      // über 50 % kommen Rot, Grün und Blau dazu (bei 100 % alle vier voll).
      w = min(c.r, min(c.g, c.b));
      c.r -= w; c.g -= w; c.b -= w;
      wOut = w * cap * min(1.0f, 2 * level);
      boost = w * cap * max(0.0f, 2 * level - 1);
#endif
      float f = cap * level;  // Farbanteil wie bisher
      uint32_t color = s->Color(min(255, (int)(c.r * f + boost + 0.5f)), min(255, (int)(c.g * f + boost + 0.5f)),
                                min(255, (int)(c.b * f + boost + 0.5f)), min(255, (int)(wOut + 0.5f)));
      for (int i = 0; i < boxCount[k][r]; i++) s->setPixelColor(boxFirst[k][r] + i, color);
    }
    int n = s->numPixels() * 4;
    if (memcmp(lastSent[k], s->getPixels(), n) != 0) {
      memcpy(lastSent[k], s->getPixels(), n);
      resend[k] = 2;
    }
    if (resend[k] == 0) continue;  // nichts geändert: Streifen in Ruhe lassen
    resend[k]--;
    s->show();
    handleUdp();  // Launchpad zwischendurch bedienen, damit es nicht warten muss
    if (lastPin >= 0 && lastPin != s->getPin()) {
      pinMode(lastPin, OUTPUT);
      digitalWrite(lastPin, LOW);
    }
    lastPin = s->getPin();
  }
}

// ---------------------------------------------------------------------
//  WLAN: Updates (Arduino IDE oder Browser http://IP/update) und Launchpad.
//  Das WLAN-Suchen stört Bluetooth. Deshalb: 2 Minuten nach dem Einschalten
//  suchen; ist der Hotspot bis dahin nicht da, wird das WLAN abgeschaltet.
//  Ist es einmal verbunden, bleibt es verbunden. Geht der Hotspot weg,
//  wird wieder 2 Minuten gesucht und dann abgeschaltet.
// ---------------------------------------------------------------------
WebServer web(80);
HTTPUpdateServer webUpdater;
bool otaStarted = false;
void bluetoothPause(bool pause);
const char *runCommand(JsonDocument &doc);  // weiter unten
String stateJson();                         // weiter unten
unsigned long lastWifiTry = 0;
unsigned long wifiUntil = 120000;  // bis dahin (ms seit Start) darf das WLAN suchen
bool wifiOff = false;

void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Verbinde mit WLAN \"%s\"", WIFI_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(250);
    Serial.print('.');
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " verbunden" : " noch nicht da, versuche weiter");
}

// HTTP: POST /api mit demselben JSON wie bei Bluetooth, z. B. {"op":"settings","mode":"rainbow"}
void sendJson(int code, const String &body) {
  web.send(code, "application/json", body);
}

void httpApi() {
  String body = web.arg("plain");  // nur gefüllt, wenn Content-Type NICHT form-urlencoded ist
  if (!body.length()) {
    sendJson(400, "{\"ok\":false,\"error\":\"leerer Body - Content-Type: application/json setzen\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    sendJson(400, "{\"ok\":false,\"error\":\"kein gueltiges JSON\"}");
    return;
  }
  const char *err = runCommand(doc);
  if (err) sendJson(400, String("{\"ok\":false,\"error\":\"") + err + "\"}");
  else sendJson(200, "{\"ok\":true}");
}

void httpState() {
  sendJson(200, stateJson());
}

// In jeder loop() aufrufen: hält WLAN und die Update-Wege am Leben.
void handleNetwork() {
  if (wifiOff) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() > wifiUntil && !Update.isRunning()) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      wifiOff = true;
      Serial.println("Kein Hotspot - WLAN aus (nur noch Bluetooth)");
    } else if (millis() - lastWifiTry > 15000) {
      lastWifiTry = millis();
      WiFi.reconnect();
    }
    return;
  }
  wifiUntil = millis() + 120000;  // verbunden: bleibt an, bei Verlust 2 Minuten weitersuchen
  if (!otaStarted) {
    otaStarted = true;
    ArduinoOTA.setHostname(HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
    // Während des Updates Bluetooth trennen, sonst bricht das Update ab
    ArduinoOTA.onStart([] { Serial.println("Update startet ..."); bluetoothPause(true); });
    ArduinoOTA.onError([](ota_error_t) { bluetoothPause(false); });
    ArduinoOTA.begin();  // startet auch mDNS: led-decke-reihe1.local
    MDNS.addService("http", "tcp", 80);
    webUpdater.setup(&web);  // Notfall-Update im Browser: http://IP/update
    web.on("/", [] { web.send(200, "text/plain", "LED-Decke laeuft. POST /api (JSON), GET /state"); });
    web.on("/api", HTTP_POST, httpApi);
    web.on("/state", HTTP_GET, httpState);
    web.begin();
    Serial.printf("WLAN ok: http://%s, Update-Name \"%s\"\n", WiFi.localIP().toString().c_str(), HOSTNAME);
  }
  ArduinoOTA.handle();
  web.handleClient();
}

// ---------------------------------------------------------------------
//  Zustand (geschützt durch mtx, weil Bluetooth in einem eigenen Task läuft)
// ---------------------------------------------------------------------
SemaphoreHandle_t mtx;
struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

CRGB boxes[ROWS][COLS];
int mode = -1;                 // -1 = eigenes Muster, -2 = Spiel vom Launchpad, sonst Index in EFFECT_IDS
CRGB gameFrame[ROWS][COLS];    // Bild, das das Launchpad (Spiele) schickt
unsigned long gameFrameAt = 0; // wann zuletzt ein Spielbild kam
int brightness = MAX_BRIGHTNESS / 2;
float speed = 1.0;
CRGB fxColor(0xff, 0x6a, 0x00);

struct Scene {
  char name[41];
  CRGB boxes[ROWS][COLS];
};
Scene scenes[MAX_SCENES];

const char *EFFECT_IDS[] = {"rainbow", "colorfade", "wave", "breathe", "sparkle", "rain", "wiring_test",
                            "music_pulse", "music_vu", "music_beat", "music_switch", "launch",
                            "fire", "aurora", "matrix", "meteor", "plasma", "comet", "ocean", "lava"};
const char *EFFECT_NAMES[] = {"Regenbogen", "Farbwechsel", "Welle", "Atmen", "Funkeln", "Regen", "Verkabelungstest",
                              "Puls", "Pegel", "Beat-Farben", "Bass-Wechsel", "Launchpad-Start",
                              "Feuer", "Polarlicht", "Matrix-Regen", "Sternschnuppen", "Plasma", "Komet",
                              "Ozean", "Lava"};
const int NUM_EFFECTS = sizeof(EFFECT_IDS) / sizeof(EFFECT_IDS[0]);
const int WIRING_TEST = 6;

Preferences prefs;
bool dirty = false, scenesDirty = false;
unsigned long lastSave = 0;
volatile int micSens = 6;  // Mikrofon-Empfindlichkeit 1 (wenig) .. 10 (sehr empfindlich)

// Nach dem Einschalten ist die Decke immer ganz weiß (halbe Helligkeit, kein Effekt).
// Nur die Szenen, das Tempo und die Effekt-Farbe bleiben gespeichert.
void loadState() {
  prefs.begin("leddecke", false);
  if (prefs.getBytesLength("scenes") == sizeof(scenes)) prefs.getBytes("scenes", scenes, sizeof(scenes));
  speed = constrain(prefs.getFloat("spd", 1.0), 0.1f, 5.0f);
  fxColor = CRGB(prefs.getUInt("col", 0xff6a00));
  micSens = constrain(prefs.getInt("sens", 6), 1, 10);
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) boxes[r][c] = WHITE;
  brightness = MAX_BRIGHTNESS / 2;
  mode = -1;
}

void saveState() {
  prefs.putFloat("spd", speed);
  prefs.putUInt("col", ((uint32_t)fxColor.r << 16) | (fxColor.g << 8) | fxColor.b);
  prefs.putInt("sens", micSens);
  if (scenesDirty) {
    prefs.putBytes("scenes", scenes, sizeof(scenes));
    scenesDirty = false;
  }
}

// ---------------------------------------------------------------------
//  Effekte  (t = Zeit in Sekunden, mit Tempo verrechnet)
// ---------------------------------------------------------------------
float t = 0;

CRGB scaled(CRGB c, float v) {
  return CRGB(c.r * v, c.g * v, c.b * v);
}

CRGB hue(float h, uint8_t s = 255, uint8_t v = 255) {
  h -= floorf(h);
  return hsv((uint8_t)(h * 255), s, v);
}

void fillFrame(CRGB c) {
  for (int r = 0; r < ROWS; r++)
    for (int c2 = 0; c2 < COLS; c2++) frame[r][c2] = c;
}

// ---------------------------------------------------------------------
//  Mikrofon: läuft nebenher und hört nur auf den Bass.
//  Vorgehen wie bei bewährten Beat-Erkennungen: Bass herausfiltern, Bass-Energie
//  alle 24 ms messen, mit der letzten Sekunde vergleichen. Ein Schlag ist, wenn
//  die Energie plötzlich deutlich über dem Durchschnitt liegt. Alle Schwellen
//  passen sich von selbst an (leises oder schwaches Mikrofon geht trotzdem).
// ---------------------------------------------------------------------
volatile float micLevel = 0;      // 0..1, wie stark der Bass gerade ist
volatile uint32_t micBeats = 0;   // zählt die Bass-Schläge
volatile uint16_t micRawMin = 0, micRawMax = 0, micBass = 0, micGate = 0;  // für die Anzeige am Handy

// Tiefpass 2. Ordnung (Butterworth); zwei hintereinander lassen nur ca. 40–150 Hz durch
struct Biquad {
  float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
  void lowpass(float fc, float fs) {
    float w = 2 * PI * fc / fs, al = sinf(w) / (2 * 0.7071f), c = cosf(w), a0 = 1 + al;
    b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
  }
  float run(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

void micTask(void *) {
  const int FS = 4000, N = 96, H = 36;   // 4000 Messungen/s, 24 ms pro Runde, 1 s Verlauf
  Biquad f1, f2;
  f1.lowpass(150, FS);
  f2.lowpass(150, FS);
  float dc = analogRead(MIC_PIN), hist[H] = {0}, quiet = 0.5f, peak = 1, prevE = 0;
  int s1 = dc, s2 = dc, hi = 0, rounds = 0, rmin = 4095, rmax = 0, shown = 0;
  unsigned long lastBeat = 0;
  for (;;) {
    float E = 0;
    unsigned long next = micros();
    for (int i = 0; i < N; i++) {
      while ((long)(micros() - next) < 0) {}
      next += 1000000 / FS;
      int v = analogRead(MIC_PIN);
      rmin = min(rmin, v); rmax = max(rmax, v);
      int m = max(min(s1, s2), min(max(s1, s2), v));  // Mittel aus 3 Werten: entfernt Störspitzen
      s1 = s2; s2 = v;
      dc += (m - dc) * 0.002f;                           // Ruhelage langsam mitlaufen lassen
      float y = f2.run(f1.run(m - dc));
      E += y * y;
    }
    E /= N;
    vTaskDelay(4);
    if (rounds < 1000) rounds++;

    // Durchschnitt und Schwankung der letzten Sekunde
    float avg = 0, var = 0;
    for (int k = 0; k < H; k++) avg += hist[k];
    avg /= H;
    for (int k = 0; k < H; k++) var += (hist[k] - avg) * (hist[k] - avg);
    float sd = sqrtf(var / H);

    // Grundgeräusch (Median, passt sich in ein paar Sekunden an) und lautester Bass zuletzt
    float u = rounds < 100 ? 0.06f : 0.02f;
    quiet = max(0.5f, quiet * (E > quiet ? 1 + u : 1 - u));
    peak = max(E, peak * 0.997f);
    float gate = quiet * 9.0f * powf(0.84f, micSens - 1);  // Empfindlichkeit 6 -> 3,8-fach

    if (rounds > H && E > gate && E > prevE && E > avg * 1.3f && E > avg + 1.2f * sd &&
        millis() - lastBeat > 200) {
      lastBeat = millis();
      micBeats++;
    }
    micLevel = (E > gate && peak > gate * 1.5f) ? constrain(logf(E / gate) / logf(peak / gate), 0.0f, 1.0f) : 0;

    hist[hi] = E; hi = (hi + 1) % H; prevE = E;
    micBass = min(65535.0f, sqrtf(E) * 10); micGate = min(65535.0f, sqrtf(gate) * 10);
    if (++shown >= 20) {  // Signal-Bereich für die Anzeige, ca. alle halbe Sekunde neu
      micRawMin = rmin; micRawMax = rmax;
      rmin = 4095; rmax = 0; shown = 0;
    }
  }
}

float sparkStart[ROWS][COLS];
uint8_t sparkHue[ROWS][COLS];
float dropStart[COLS];
float dropWait[COLS];


// ---------------------------------------------------------------------
//  Neue Effekte (ab Index 12): Hilfsfunktionen und Zustand
// ---------------------------------------------------------------------
CRGB mixColor(CRGB a, CRGB b, float k) {  // k = 0 -> a, k = 1 -> b
  k = constrain(k, 0.0f, 1.0f);
  return CRGB(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k);
}

// Hitze 0..1 -> schwarz, rot, orange, gelb, weiß
CRGB heatColor(float h) {
  h = constrain(h, 0.0f, 1.0f) * 3;
  return CRGB(255 * min(1.0f, h), 255 * constrain(h - 1, 0.0f, 1.0f), 255 * constrain(h - 2, 0.0f, 1.0f));
}

float fireHeat[COLS][ROWS];   // Index 0 = unten
float fireLastT = 0;
float matrixPos[COLS], matrixSpd[COLS];
bool matrixInit = false;
float matrixLastT = 0;
const int NUM_METEORS = 4;
float meteorT0[NUM_METEORS], meteorRow[NUM_METEORS], meteorCol[NUM_METEORS], meteorSpd[NUM_METEORS];
bool meteorInit = false;

void spawnMeteor(int i, float now) {
  meteorT0[i] = now;
  meteorRow[i] = -random(0, 40) / 10.0f;
  meteorCol[i] = random(0, (COLS + 6) * 10) / 10.0f;
  meteorSpd[i] = 5.0f + random(0, 40) / 10.0f;
}

void renderNewEffect(int m) {
  switch (m) {
    case 12: {  // Feuer: von unten aufsteigende Hitze (feste Feuerfarben)
      int steps = (int)((t - fireLastT) * 20);
      if (steps > 4) steps = 4;
      if (steps > 0) fireLastT = t;
      for (int s = 0; s < steps; s++)
        for (int c = 0; c < COLS; c++) {
          float *h = fireHeat[c];
          for (int k = 0; k < ROWS; k++) h[k] = max(0.0f, h[k] - random(0, 60) / 255.0f);
          for (int k = ROWS - 1; k >= 2; k--) h[k] = (h[k - 1] + h[k - 2] + h[k - 2]) / 3.0f;
          if (random(255) < 130) {
            int y = random(min(3, ROWS));
            h[y] = min(1.0f, h[y] + random(160, 255) / 255.0f);
          }
        }
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) frame[r][c] = heatColor(fireHeat[c][ROWS - 1 - r]);
      break;
    }
    case 13:  // Polarlicht: grün-violette Schleier wandern (feste Farben)
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float a = (sinf(c * 0.8f + t * 0.5f + sinf(r * 0.45f + t * 0.3f) * 1.6f) + 1) / 2;
          float b = (sinf(c * 0.35f - t * 0.27f + r * 0.3f) + 1) / 2;
          float v = 0.04f + 0.96f * a * a;
          frame[r][c] = hue(0.30f + 0.42f * b, 255, 255 * v);
        }
      break;
    case 14: {  // Matrix-Regen: helle Köpfe mit Schweif in der Effekt-Farbe fallen nach unten
      if (!matrixInit) {
        matrixInit = true;
        for (int c = 0; c < COLS; c++) {
          matrixPos[c] = -random(0, 100) / 10.0f;
          matrixSpd[c] = 3.0f + random(0, 40) / 10.0f;
        }
      }
      float dt = t - matrixLastT;
      matrixLastT = t;
      if (dt < 0 || dt > 1) dt = 0;
      fillFrame(BLACK);
      for (int c = 0; c < COLS; c++) {
        matrixPos[c] += dt * matrixSpd[c];
        if (matrixPos[c] > ROWS + 6) {
          matrixPos[c] = -random(0, 30) / 10.0f;
          matrixSpd[c] = 3.0f + random(0, 40) / 10.0f;
        }
        for (int r = 0; r < ROWS; r++) {
          float d = matrixPos[c] - r;  // Abstand hinter dem Kopf
          if (d < 0 || d > 6) continue;
          frame[r][c] = d < 1 ? mixColor(fxColor, WHITE, 0.7f) : scaled(fxColor, (1 - d / 6) * (0.6f + 0.4f * random(100) / 100.0f));
        }
      }
      break;
    }
    case 15: {  // Sternschnuppen: weiße Streifen mit Schweif ziehen schräg über die Decke
      if (!meteorInit) {
        meteorInit = true;
        for (int i = 0; i < NUM_METEORS; i++) spawnMeteor(i, t);
      }
      fillFrame(BLACK);
      for (int i = 0; i < NUM_METEORS; i++) {
        float age = t - meteorT0[i];
        float hr = meteorRow[i] + age * meteorSpd[i];            // läuft nach unten ...
        float hc = meteorCol[i] - age * meteorSpd[i] * 0.5f;     // ... und nach links
        if (hr > ROWS + 7) {
          spawnMeteor(i, t);
          continue;
        }
        const float DR = 0.894f, DC = -0.447f;  // Flugrichtung (normiert)
        for (int r = 0; r < ROWS; r++)
          for (int c = 0; c < COLS; c++) {
            float dr = r - hr, dc = c - hc;
            float behind = -(dr * DR + dc * DC);
            float side = fabsf(dr * DC - dc * DR);
            if (behind < -0.3f || behind > 6 || side > 0.8f) continue;
            float v = (1 - max(0.0f, behind) / 6) * (1 - side / 0.8f);
            CRGB col = mixColor(WHITE, fxColor, behind / 6);
            CRGB &px = frame[r][c];
            CRGB add = scaled(col, v);
            px = CRGB(min(255, px.r + add.r), min(255, px.g + add.g), min(255, px.b + add.b));
          }
      }
      break;
    }
    case 16:  // Plasma: ineinanderfließende Farben (Regenbogen-Farben)
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float v = sinf(c * 0.9f + t) + sinf(r * 0.6f - t * 1.3f) + sinf((c + r) * 0.5f + t * 0.7f) +
                    sinf(sqrtf((c - 3.0f) * (c - 3.0f) + (r - 4.0f) * (r - 4.0f)) * 0.9f - t);
          frame[r][c] = hue(v / 8 + 0.5f + t * 0.03f);
        }
      break;
    case 17: {  // Komet: ein Lichtpunkt mit Schweif läuft im Zickzack durch alle Reihen
      const int N = ROWS * COLS, TAIL = 14;
      int head = (int)(t * 12) % N;
      fillFrame(BLACK);
      for (int k = TAIL; k >= 0; k--) {
        int i = ((head - k) % N + N) % N;
        int col = i / ROWS, row = (col % 2 == 0) ? i % ROWS : ROWS - 1 - i % ROWS;
        frame[row][col] = k == 0 ? WHITE : scaled(fxColor, powf(1 - (float)k / (TAIL + 1), 1.5f));
      }
      break;
    }
    case 18:  // Ozean: Wellen in der Effekt-Farbe, auf den Kämmen weißer Schaum
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float w1 = (sinf(r * 0.7f - t * 1.2f + c * 0.35f) + 1) / 2;
          float w2 = (sinf(r * 0.4f + t * 0.8f - c * 0.5f) + 1) / 2;
          float v = 0.15f + 0.85f * (0.65f * w1 + 0.35f * w2);
          CRGB col = scaled(fxColor, v);
          frame[r][c] = v > 0.88f ? mixColor(col, WHITE, (v - 0.88f) * 6) : col;
        }
      break;
    case 19:  // Lava: langsame glühende Blasen (feste Lavafarben)
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float n = sinf(c * 0.6f + t * 0.3f) + sinf(r * 0.45f - t * 0.25f + sinf(c * 0.5f + t * 0.2f)) +
                    sinf((r + c) * 0.4f + t * 0.15f);
          float v = (n + 3) / 6;
          frame[r][c] = heatColor(0.08f + 0.62f * v * v);
        }
      break;
  }
}

void renderEffect() {
  if (mode >= 12) {
    renderNewEffect(mode);
    return;
  }
  switch (mode) {
    case 0:  // Regenbogen
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) frame[r][c] = hue(t * 0.1f + (float)(r + c) / (ROWS + COLS));
      break;
    case 1:  // Farbwechsel
      fillFrame(hue(t * 0.05f));
      break;
    case 2:  // Welle
      for (int r = 0; r < ROWS; r++) {
        float v = 0.08f + 0.92f * (sinf(r * 0.8f - t * 2.5f) + 1) / 2;
        for (int c = 0; c < COLS; c++) frame[r][c] = scaled(fxColor, v);
      }
      break;
    case 3:  // Atmen
      fillFrame(scaled(fxColor, 0.05f + 0.95f * (sinf(t * 1.5f) + 1) / 2));
      break;
    case 4: {  // Funkeln
      if (random(2) == 0) {
        int r = random(ROWS), c = random(COLS);
        sparkStart[r][c] = t;
        sparkHue[r][c] = random(256);
      }
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float age = t - sparkStart[r][c];
          frame[r][c] = (age >= 0 && age < 1.5f) ? hsv(sparkHue[r][c], 150, 255 * (1 - age / 1.5f)) : BLACK;
        }
      break;
    }
    case 5:  // Regen
      fillFrame(BLACK);
      for (int c = 0; c < COLS; c++) {
        if (t - dropStart[c] > dropWait[c] || t < dropStart[c]) {
          dropStart[c] = t - random(100) / 100.0f;
          dropWait[c] = ROWS / 4.0f + random(200) / 100.0f;
        }
        float head = (t - dropStart[c]) * 4;
        for (int r = 0; r < ROWS; r++) {
          float d = head - r;
          if (d >= 0 && d < 4) frame[r][c] = scaled(fxColor, 1 - d / 4);
        }
      }
      break;
    case 7: {  // Musik: Puls – die Effekt-Farbe wird mit der Musik hell und dunkel
      static float shown = 0;
      shown = max((float)micLevel, shown * 0.85f);
      fillFrame(scaled(fxColor, 0.03f + 0.97f * shown));
      break;
    }
    case 8: {  // Musik: Pegel – Balken von unten, grün -> gelb -> rot
      static float shown = 0;
      shown = max((float)micLevel, shown * 0.9f);
      float height = shown * ROWS;
      for (int r = 0; r < ROWS; r++) {
        int fromBottom = ROWS - 1 - r;
        CRGB c = hue(0.33f * (1 - (float)fromBottom / (ROWS - 1)));
        float v = constrain(height - fromBottom, 0.0f, 1.0f);
        for (int col = 0; col < COLS; col++) frame[r][col] = scaled(c, v);
      }
      break;
    }
    case 9: {  // Musik: Beat-Farben – bei jedem Bass-Schlag sofort eine neue Farbe
      static uint32_t seen = 0;
      static float glow = 1, h = 0;
      if (micBeats != seen) {
        seen = micBeats;
        h += 0.2f + random(600) / 1000.0f;   // deutlich andere Farbe als vorher
        glow = 1;
      }
      glow = max(0.6f, glow * 0.92f);        // kurz heller beim Schlag, dann ruhig
      fillFrame(hue(h, 255, 255 * glow));
      break;
    }
    case 10: {  // Musik: Bass-Wechsel – jeder Bass-Schlag schaltet sofort auf die nächste kräftige Farbe
      static const CRGB COLORS[] = {CRGB(255, 0, 0), CRGB(0, 0, 255), CRGB(0, 255, 0), CRGB(255, 0, 180),
                                    CRGB(255, 200, 0), CRGB(0, 220, 255), CRGB(140, 0, 255), CRGB(255, 80, 0)};
      static uint32_t seen = 0;
      static int i = 0;
      if (micBeats != seen) {
        seen = micBeats;
        i = (i + 1 + random(6)) % 8;   // nie zweimal dieselbe Farbe
      }
      fillFrame(COLORS[i]);
      break;
    }
    case 11: {  // Launchpad-Start: bunte Welle läuft schräg über die Decke und verglüht, abwechselnd von beiden Ecken
      const float SPEED = 8;                          // Kästen pro Sekunde
      const float span = ROWS + COLS - 2 + 8;         // Weg der Welle plus Nachglühen
      int n = (int)(t * SPEED / span);
      float head = t * SPEED - n * span;
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float d = (n % 2) ? (ROWS - 1 - r) + (COLS - 1 - c) : r + c;
          float age = (head - d) / SPEED;             // Sekunden, seit die Welle hier war
          frame[r][c] = age < 0 ? BLACK : hue(d / (ROWS + COLS) + n * 0.17f, 255, 255 * expf(-age * 2.5f));
        }
      break;
    }
    case WIRING_TEST: {  // Reihe für Reihe von oben nach unten, oberster Kasten rot
      fillFrame(BLACK);
      int step = (int)(t * 2) % (COLS * ROWS);
      int col = step / ROWS, row = step % ROWS;
      frame[row][col] = (row == 0) ? RED : WHITE;
      break;
    }
  }
}

// ---------------------------------------------------------------------
//  Befehle von der Steuerseite (per Bluetooth).
//  Rückgabe: nullptr = ok, sonst Fehlertext.
// ---------------------------------------------------------------------
bool parseHex(const char *s, CRGB &out) {
  if (!s || strlen(s) != 7 || s[0] != '#') return false;
  for (int i = 1; i < 7; i++)
    if (!isxdigit((unsigned char)s[i])) return false;
  out = CRGB((uint32_t)strtoul(s + 1, nullptr, 16));
  return true;
}

const char *cmdBoxes(JsonDocument &doc) {
  CRGB col;
  if (!parseHex(doc["color"] | "", col)) return "color";
  Lock l;
  for (JsonArray cell : doc["cells"].as<JsonArray>()) {
    int r = cell[0] | -1, c = cell[1] | -1;
    if (r >= 0 && r < ROWS && c >= 0 && c < COLS) boxes[r][c] = col;
  }
  mode = -1;
  dirty = true;
  return nullptr;
}

const char *cmdFill(JsonDocument &doc) {
  CRGB col;
  if (!parseHex(doc["color"] | "", col)) return "color";
  Lock l;
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) boxes[r][c] = col;
  mode = -1;
  dirty = true;
  return nullptr;
}

const char *cmdSettings(JsonDocument &doc) {
  Lock l;
  if (doc["brightness"].is<int>()) brightness = constrain(doc["brightness"].as<int>(), 0, MAX_BRIGHTNESS);
  if (doc["speed"].is<float>()) speed = constrain(doc["speed"].as<float>(), 0.1f, 5.0f);
  if (doc["color"].is<const char *>() && !parseHex(doc["color"], fxColor)) return "color";
  if (doc["mic_sens"].is<int>()) micSens = constrain(doc["mic_sens"].as<int>(), 1, 10);
  if (doc["mode"].is<const char *>()) {
    const char *m = doc["mode"];
    int found = strcmp(m, "static") == 0 ? -1 : -2;
    for (int i = 0; i < NUM_EFFECTS; i++)
      if (strcmp(m, EFFECT_IDS[i]) == 0) found = i;
    if (found == -2) return "mode";
    mode = found;
  }
  dirty = true;
  return nullptr;
}

// op: 's' speichern, 'l' laden, 'd' löschen
const char *cmdScene(char op, String name) {
  name = name.substring(0, 40);
  if (!name.length()) return "scene";
  Lock l;
  int i = -1;
  for (int k = 0; k < MAX_SCENES; k++)
    if (scenes[k].name[0] && name == scenes[k].name) i = k;
  if (op == 's') {
    for (int k = 0; k < MAX_SCENES && i < 0; k++)
      if (!scenes[k].name[0]) i = k;
    if (i < 0) return "Speicher voll";
    strlcpy(scenes[i].name, name.c_str(), sizeof(scenes[i].name));
    memcpy(scenes[i].boxes, boxes, sizeof(boxes));
  } else if (op == 'l') {
    if (i < 0) return "scene";
    memcpy(boxes, scenes[i].boxes, sizeof(boxes));
    mode = -1;
  } else if (op == 'd') {
    if (i >= 0) scenes[i].name[0] = 0;
  }
  scenesDirty = dirty = true;
  return nullptr;
}

String sceneNamesJson() {  // Aufrufer hält den Lock
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < MAX_SCENES; i++)
    if (scenes[i].name[0]) a.add(scenes[i].name);
  String out;
  serializeJson(d, out);
  return out;
}

// ---------------------------------------------------------------------
//  Bluetooth (BLE)
// ---------------------------------------------------------------------
NimBLECharacteristic *bleState, *bleScenes, *bleMic;
volatile unsigned long lastBleActivity = 0;  // zuletzt kam ein Befehl / wurde etwas gelesen
volatile unsigned long lastBleClient = 0;    // zuletzt war jemand verbunden
volatile bool bleRestartRequested = false;   // wird in loop() ausgeführt
uint32_t bleRestarts = 0;

// Zustand binär: [1, mode, helligkeit, max, tempo*10, r,g,b, rows, cols, boxes..., frame...]
const size_t STATE_SIZE = 10 + ROWS * COLS * 6;

void buildState(uint8_t *buf) {
  Lock l;
  buf[0] = 1;
  buf[1] = (uint8_t)(int8_t)mode;
  buf[2] = brightness;
  buf[3] = MAX_BRIGHTNESS;
  buf[4] = (uint8_t)roundf(speed * 10);
  buf[5] = fxColor.r; buf[6] = fxColor.g; buf[7] = fxColor.b;
  buf[8] = ROWS; buf[9] = COLS;
  memcpy(buf + 10, boxes, ROWS * COLS * 3);
  memcpy(buf + 10 + ROWS * COLS * 3, frame, ROWS * COLS * 3);
}

// Spielbild vom Launchpad: {"op":"frame","px":"rrggbb..."} (63 Kästen, Reihe für Reihe)
const char *cmdFrame(JsonDocument &doc) {
  const char *px = doc["px"] | "";
  if (strlen(px) != ROWS * COLS * 6) return "px";
  Lock l;
  for (int i = 0; i < ROWS * COLS; i++) {
    char h[7];
    memcpy(h, px + i * 6, 6);
    h[6] = 0;
    gameFrame[i / COLS][i % COLS] = CRGB((uint32_t)strtoul(h, nullptr, 16));
  }
  mode = -2;
  gameFrameAt = millis();
  return nullptr;  // nicht speichern (kein Flash-Schreiben bei Spielen)
}

// Befehle als JSON: {"op":"boxes"|"fill"|"settings"|"frame"|"scene_save"|"scene_load"|"scene_delete"|"get", ...}
// Rückgabe: nullptr = ok, sonst Fehlertext (Bluetooth und UDP ignorieren ihn, HTTP meldet ihn zurück)
const char *runCommand(JsonDocument &doc) {
  String op = doc["op"] | "";
  if (op == "get") return nullptr;
  if (op == "ble_restart") { bleRestartRequested = true; return nullptr; }  // Bluetooth neu starten
  if (op == "boxes") return cmdBoxes(doc);
  if (op == "fill") return cmdFill(doc);
  if (op == "settings") return cmdSettings(doc);
  if (op == "frame") return cmdFrame(doc);
  if (op == "scene_save") return cmdScene('s', doc["name"] | "");
  if (op == "scene_load") return cmdScene('l', doc["name"] | "");
  if (op == "scene_delete") return cmdScene('d', doc["name"] | "");
  return "op";
}

// Zustand als lesbares JSON (für GET /state)
String stateJson() {
  JsonDocument d;
  {
    Lock l;
    d["mode"] = mode == -1 ? "static" : mode == -2 ? "game" : EFFECT_IDS[mode];
    d["brightness"] = brightness;
    d["max_brightness"] = MAX_BRIGHTNESS;
    d["speed"] = speed;
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", fxColor.r, fxColor.g, fxColor.b);
    d["color"] = hex;
    d["mic_sens"] = micSens;
    JsonArray sc = d["scenes"].to<JsonArray>();
    for (int i = 0; i < MAX_SCENES; i++)
      if (scenes[i].name[0]) sc.add(scenes[i].name);
  }
  JsonArray ef = d["effects"].to<JsonArray>();
  for (int i = 0; i < NUM_EFFECTS; i++) ef.add(EFFECT_IDS[i]);
  d["wifi_rssi"] = WiFi.RSSI();
  NimBLEServer *srv = NimBLEDevice::getServer();
  d["ble_clients"] = srv ? srv->getConnectedCount() : 0;
  d["ble_advertising"] = srv ? NimBLEDevice::getAdvertising()->isAdvertising() : false;
  d["ble_restarts"] = bleRestarts;
  d["ble_idle_s"] = (millis() - lastBleClient) / 1000;
  d["free_heap"] = ESP.getFreeHeap();
  d["min_free_heap"] = ESP.getMinFreeHeap();
  d["uptime_s"] = millis() / 1000;
  String out;
  serializeJson(d, out);
  return out;
}

class StateCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    lastBleActivity = millis();
    static uint8_t buf[STATE_SIZE];
    buildState(buf);
    c->setValue(buf, sizeof(buf));
  }
};

class ScenesCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    Lock l;
    c->setValue(sceneNamesJson());
  }
};

// Mikrofon-Anzeige: [1, Empfindlichkeit, Pegel 0..255, Schläge, Signal min, Signal max, Bass, Schwelle] (je 2 Byte)
class MicCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    uint8_t b[12];
    uint16_t w[4] = {micRawMin, micRawMax, micBass, micGate};
    b[0] = 1; b[1] = micSens; b[2] = micLevel * 255; b[3] = micBeats & 0xff;
    memcpy(b + 4, w, sizeof(w));
    c->setValue(b, sizeof(b));
  }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    lastBleActivity = millis();
    JsonDocument doc;
    NimBLEAttValue v = c->getValue();
    if (deserializeJson(doc, (const char *)v.data(), v.length())) return;
    runCommand(doc);
  }
};

// Nach jeder Verbindung weiter sichtbar bleiben: Handy und Raspberry Pi gleichzeitig
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, NimBLEConnInfo &info) override {
    lastBleClient = lastBleActivity = millis();
    if (srv->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) NimBLEDevice::startAdvertising();
  }
};

bool blePaused = false;  // während eines WLAN-Updates bleibt Bluetooth aus
void bluetoothPause(bool pause) {
  blePaused = pause;
  NimBLEServer *srv = NimBLEDevice::getServer();
  if (!srv) return;
  srv->advertiseOnDisconnect(!pause);
  if (pause) {
    NimBLEDevice::stopAdvertising();
    for (uint16_t h : srv->getPeerDevices()) srv->disconnect(h);
  } else {
    NimBLEDevice::startAdvertising();
  }
}

void startBle() {
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setMTU(517);
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCallbacks());
  srv->advertiseOnDisconnect(true);
  NimBLEService *svc = srv->createService(BLE_SERVICE_UUID);
  svc->createCharacteristic(BLE_CMD_UUID, NIMBLE_PROPERTY::WRITE, 512)->setCallbacks(new CmdCallbacks());
  bleState = svc->createCharacteristic(BLE_STATE_UUID, NIMBLE_PROPERTY::READ, 512);
  bleState->setCallbacks(new StateCallbacks());
  bleScenes = svc->createCharacteristic(BLE_SCENES_UUID, NIMBLE_PROPERTY::READ, 512);
  bleScenes->setCallbacks(new ScenesCallbacks());
  bleMic = svc->createCharacteristic(BLE_MIC_UUID, NIMBLE_PROPERTY::READ, 16);
  bleMic->setCallbacks(new MicCallbacks());
  svc->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(BLE_NAME);
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();
  lastBleClient = lastBleActivity = millis();
  Serial.printf("Bluetooth bereit: \"%s\"\n", BLE_NAME);
}

// Bluetooth komplett abbauen und neu aufbauen (letzter Ausweg bei hängendem Bluetooth)
void restartBle(const char *why) {
  Serial.printf("Bluetooth wird neu gestartet (%s)\n", why);
  NimBLEDevice::deinit(true);
  delay(300);
  startBle();
  bleRestarts++;
}

// Alle 3 Sekunden aus loop(): Bluetooth am Leben halten
void bleMaintenance() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck < 3000) return;
  lastCheck = now;
  if (blePaused || Update.isRunning()) return;
  if (bleRestartRequested) {
    bleRestartRequested = false;
    restartBle("angefordert");
    return;
  }
  NimBLEServer *srv = NimBLEDevice::getServer();
  if (!srv) {
    restartBle("kein Server");
    return;
  }
  int n = srv->getConnectedCount();
  if (n > 0) lastBleClient = now;
  // Solange ein Platz frei ist, muss die Decke sichtbar und verbindbar sein
  if (n < CONFIG_BT_NIMBLE_MAX_CONNECTIONS && !NimBLEDevice::getAdvertising()->isAdvertising())
    NimBLEDevice::startAdvertising();
  // Alle Plätze belegt, aber lange nichts passiert: Geisterverbindungen trennen
  if (n >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS && now - lastBleActivity > BLE_GHOST_MS) {
    Serial.println("Bluetooth: Geisterverbindungen werden getrennt");
    for (uint16_t h : srv->getPeerDevices()) srv->disconnect(h);
    lastBleActivity = now;
  }
  // Lange niemand verbunden: Bluetooth vorsorglich neu starten
  if (BLE_IDLE_RESTART_MS > 0 && n == 0 && now - lastBleClient > BLE_IDLE_RESTART_MS) restartBle("lange ohne Verbindung");
}
// ---------------------------------------------------------------------
//  Launchpad am Raspberry Pi: dieselben Befehle per WLAN (UDP).
//  Jede Nachricht wird mit dem aktuellen Zustand beantwortet.
// ---------------------------------------------------------------------
WiFiUDP udp;
bool udpStarted = false;

void handleUdp() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!udpStarted) udpStarted = udp.begin(UDP_PORT);
  if (!udpStarted) return;
  static char in[512];
  static uint8_t out[STATE_SIZE];
  for (int k = 0; k < 4; k++) {
    if (udp.parsePacket() <= 0) return;
    int n = udp.read((uint8_t *)in, sizeof(in));
    JsonDocument doc;
    if (n <= 0 || deserializeJson(doc, in, n)) continue;
    runCommand(doc);  // {"op":"get"} fragt nur den Zustand ab
    buildState(out);
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write(out, STATE_SIZE);
    udp.endPacket();
  }
}


// ---------------------------------------------------------------------
//  Hauptschleife
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  mtx = xSemaphoreCreateMutex();
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) sparkStart[r][c] = -10;
  addLeds();
  loadState();
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
  xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 1, nullptr, 0);
  memcpy(frame, boxes, sizeof(frame));  // sofort weiß
  shownBrightness = brightness;
  showAll();
  startBle();  // Bluetooth zuerst, damit Pi und Handy nicht auf das WLAN warten
  startWifi();
  Serial.printf("\nLED-Decke: 14 Streifen x %d LEDs\n", LEDS_PER_STRIP);
}

void loop() {
  static unsigned long lastFrame = 0;
  bleMaintenance();
  handleNetwork();
  handleUdp();
  unsigned long now = millis();
  if (now - lastFrame >= 1000 / FPS) {
    {
      Lock l;
      t += (now - lastFrame) / 1000.0f * speed;
      if (mode == -2 && now - gameFrameAt > 5000) mode = -1;  // Spiel vorbei/Launchpad weg
      if (mode == -2) memcpy(frame, gameFrame, sizeof(frame));
      else if (mode < 0) memcpy(frame, boxes, sizeof(frame));
      else renderEffect();
      shownBrightness = brightness;
      if (dirty && now - lastSave > 3000) {
        saveState();
        dirty = false;
        lastSave = now;
      }
    }
    lastFrame = now;
    showAll();
  }
  delay(1);
}
