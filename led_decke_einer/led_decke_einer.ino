// LED-Decke – Firmware für EINEN ESP32, der alle 14 Streifen steuert
//
// Der ESP32 verbindet sich per Bluetooth mit der Steuerseite am Handy und mit
// dem Raspberry Pi (Launchpad). Nur Updates gehen per WLAN (OTA).
// Einstellungen stehen direkt hier unten.

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
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

#define LED_PIXEL_TYPE NEO_GRBW
#define USE_WHITE_CHANNEL true
#define MAX_BRIGHTNESS 160
#define FPS 20
#define MAX_SCENES 10
// =============================================================

#define BLE_SERVICE_UUID "6e1d0001-8a3c-4c5b-9b1e-1ed0c0dec0de"
#define BLE_CMD_UUID     "6e1d0002-8a3c-4c5b-9b1e-1ed0c0dec0de"  // schreiben: JSON-Befehl
#define BLE_STATE_UUID   "6e1d0003-8a3c-4c5b-9b1e-1ed0c0dec0de"  // lesen: Zustand (binär)
#define BLE_SCENES_UUID  "6e1d0004-8a3c-4c5b-9b1e-1ed0c0dec0de"  // lesen: Szenen-Namen (JSON)

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
void showAll() {
  for (int k = 0; k < NUM_STRIPS; k++) {
    Adafruit_NeoPixel *s = strips[k];
    s->setBrightness(shownBrightness);
    for (int r = 0; r < ROWS; r++) {
      CRGB c = frame[r][stripCol[k]];
      uint8_t w = 0;
#if USE_WHITE_CHANNEL
      // Der gemeinsame Weiß-Anteil geht auf die weiße LED (heller, schöneres Weiß)
      w = min(c.r, min(c.g, c.b));
      c.r -= w; c.g -= w; c.b -= w;
#endif
      uint32_t color = s->Color(c.r, c.g, c.b, w);
      for (int i = 0; i < boxCount[k][r]; i++) s->setPixelColor(boxFirst[k][r] + i, color);
    }
    s->show();
    handleUdp();  // Launchpad zwischendurch bedienen, damit es nicht warten muss
    int prev = (k + NUM_STRIPS - 1) % NUM_STRIPS;
    pinMode(strips[prev]->getPin(), OUTPUT);
    digitalWrite(strips[prev]->getPin(), LOW);
  }
}

// ---------------------------------------------------------------------
//  WLAN: Updates (Arduino IDE oder Browser http://IP/update) und Launchpad.
//  Das WLAN-Suchen stört Bluetooth stark. Deshalb: Ist das WLAN 2 Minuten nach
//  dem Einschalten nicht da (oder 1 Minute weg), wird es ganz abgeschaltet.
//  Für ein Update: Hotspot an, dann die Decke einmal aus- und einschalten.
// ---------------------------------------------------------------------
WebServer web(80);
HTTPUpdateServer webUpdater;
bool otaStarted = false;
void bluetoothPause(bool pause);
unsigned long lastWifiTry = 0;
unsigned long wifiUntil = 120000;  // so lange (ms seit Start) darf das WLAN suchen
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

// In jeder loop() aufrufen: hält WLAN und die Update-Wege am Leben.
void handleNetwork() {
  if (wifiOff) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() > wifiUntil) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      wifiOff = true;
      Serial.println("WLAN aus (nur Bluetooth)");
    } else if (millis() - lastWifiTry > 15000) {
      lastWifiTry = millis();
      WiFi.reconnect();
    }
    return;
  }
  wifiUntil = millis() + 60000;
  if (!otaStarted) {
    otaStarted = true;
    ArduinoOTA.setHostname(HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
    // Während des Updates Bluetooth trennen, sonst bricht das Update ab
    ArduinoOTA.onStart([] { Serial.println("Update startet ..."); bluetoothPause(true); });
    ArduinoOTA.onError([](ota_error_t) { bluetoothPause(false); });
    ArduinoOTA.begin();
    webUpdater.setup(&web);  // Notfall-Update im Browser: http://IP/update
    web.on("/", [] { web.send(200, "text/plain", "LED-Decke laeuft"); });
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
int mode = -1;                 // -1 = eigenes Muster, sonst Index in EFFECT_IDS
int brightness = MAX_BRIGHTNESS / 2;
float speed = 1.0;
CRGB fxColor(0xff, 0x6a, 0x00);

struct Scene {
  char name[41];
  CRGB boxes[ROWS][COLS];
};
Scene scenes[MAX_SCENES];

const char *EFFECT_IDS[] = {"rainbow", "colorfade", "wave", "breathe", "sparkle", "rain", "wiring_test"};
const char *EFFECT_NAMES[] = {"Regenbogen", "Farbwechsel", "Welle", "Atmen", "Funkeln", "Regen", "Verkabelungstest"};
const int NUM_EFFECTS = sizeof(EFFECT_IDS) / sizeof(EFFECT_IDS[0]);
const int WIRING_TEST = 6;

Preferences prefs;
bool dirty = false, scenesDirty = false;
unsigned long lastSave = 0;

// Nach dem Einschalten ist die Decke immer ganz weiß (halbe Helligkeit, kein Effekt).
// Nur die Szenen, das Tempo und die Effekt-Farbe bleiben gespeichert.
void loadState() {
  prefs.begin("leddecke", false);
  if (prefs.getBytesLength("scenes") == sizeof(scenes)) prefs.getBytes("scenes", scenes, sizeof(scenes));
  speed = constrain(prefs.getFloat("spd", 1.0), 0.1f, 5.0f);
  fxColor = CRGB(prefs.getUInt("col", 0xff6a00));
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) boxes[r][c] = WHITE;
  brightness = MAX_BRIGHTNESS / 2;
  mode = -1;
}

void saveState() {
  prefs.putFloat("spd", speed);
  prefs.putUInt("col", ((uint32_t)fxColor.r << 16) | (fxColor.g << 8) | fxColor.b);
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

float sparkStart[ROWS][COLS];
uint8_t sparkHue[ROWS][COLS];
float dropStart[COLS];
float dropWait[COLS];

void renderEffect() {
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
NimBLECharacteristic *bleState, *bleScenes;

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

// Befehle als JSON: {"op":"boxes"|"fill"|"settings"|"scene_save"|"scene_load"|"scene_delete"|"get", ...}
void runCommand(JsonDocument &doc) {
  String op = doc["op"] | "";
  if (op == "boxes") cmdBoxes(doc);
  else if (op == "fill") cmdFill(doc);
  else if (op == "settings") cmdSettings(doc);
  else if (op == "scene_save") cmdScene('s', doc["name"] | "");
  else if (op == "scene_load") cmdScene('l', doc["name"] | "");
  else if (op == "scene_delete") cmdScene('d', doc["name"] | "");
}

class StateCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
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

class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    JsonDocument doc;
    NimBLEAttValue v = c->getValue();
    if (deserializeJson(doc, (const char *)v.data(), v.length())) return;
    runCommand(doc);
  }
};

// Nach jeder Verbindung weiter sichtbar bleiben: Handy und Raspberry Pi gleichzeitig
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, NimBLEConnInfo &info) override {
    if (srv->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) NimBLEDevice::startAdvertising();
  }
};

void bluetoothPause(bool pause) {
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
  svc->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(BLE_NAME);
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();
  Serial.printf("Bluetooth bereit: \"%s\"\n", BLE_NAME);
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
  memcpy(frame, boxes, sizeof(frame));  // sofort weiß
  shownBrightness = brightness;
  showAll();
  startBle();  // Bluetooth zuerst, damit Pi und Handy nicht auf das WLAN warten
  startWifi();
  Serial.printf("\nLED-Decke: 14 Streifen x %d LEDs\n", LEDS_PER_STRIP);
}

void loop() {
  static unsigned long lastFrame = 0;
  handleNetwork();
  handleUdp();
  unsigned long now = millis();
  if (now - lastFrame >= 1000 / FPS) {
    {
      Lock l;
      t += (now - lastFrame) / 1000.0f * speed;
      if (mode < 0) memcpy(frame, boxes, sizeof(frame));
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
