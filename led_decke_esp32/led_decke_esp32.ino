// LED-Decke – Firmware für 7 ESP32 (einer pro Reihe)
//
// Der Chef-ESP32 (MASTER_ROW) verbindet sich per Bluetooth mit der Steuerseite
// am Handy, berechnet jedes Bild für die ganze Decke und schickt es per ESP-NOW
// an alle. Jeder ESP32 zeigt davon nur seine eigene Reihe. Weil alle dasselbe
// Bild bekommen, laufen die Effekte automatisch synchron.
// Einstellungen in config.h – pro ESP32 nur ROW_NUMBER ändern.

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#include "config.h"

#define IS_MASTER (ROW_NUMBER == MASTER_ROW)
static_assert(ROW_NUMBER >= 1 && ROW_NUMBER <= COLS, "ROW_NUMBER muss 1 bis 7 sein");

#if IS_MASTER
// Diese UUIDs stehen genauso in der Steuerseite – nur gemeinsam ändern.
#define BLE_SERVICE_UUID "6e1d0001-8a3c-4c5b-9b1e-1ed0c0dec0de"
#define BLE_CMD_UUID     "6e1d0002-8a3c-4c5b-9b1e-1ed0c0dec0de"  // schreiben: JSON-Befehl
#define BLE_STATE_UUID   "6e1d0003-8a3c-4c5b-9b1e-1ed0c0dec0de"  // lesen: Zustand (binär)
#define BLE_SCENES_UUID  "6e1d0004-8a3c-4c5b-9b1e-1ed0c0dec0de"  // lesen: Szenen-Namen (JSON)
#endif

// ---------------------------------------------------------------------
//  LEDs dieser Reihe
// ---------------------------------------------------------------------
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

const int MY_COL = ROW_NUMBER - 1;
const int NUM_STRIPS = 2;  // Streifen A und B, jeder an seinem eigenen Pin
Adafruit_NeoPixel stripA(LEDS_PER_STRIP, STRIP_A_PIN, LED_PIXEL_TYPE + NEO_KHZ800);
Adafruit_NeoPixel stripB(LEDS_PER_STRIP, STRIP_B_PIN, LED_PIXEL_TYPE + NEO_KHZ800);
Adafruit_NeoPixel *strips[NUM_STRIPS] = {&stripA, &stripB};

uint16_t boxFirst[ROWS][NUM_STRIPS];  // erste LED jedes Streifens im Kasten
uint16_t boxCount[ROWS][NUM_STRIPS];  // Anzahl LEDs im Kasten

// Streifen gleichmäßig auf die Kästen verteilen (83 LEDs / 9 Kästen -> 9 oder 10)
void buildMap() {
  const bool startTop[NUM_STRIPS][COLS] = {STRIP_A_STARTS_AT_TOP, STRIP_B_STARTS_AT_TOP};
  for (int k = 0; k < NUM_STRIPS; k++) {
    bool down = startTop[k][MY_COL];
    for (int i = 0; i < ROWS; i++) {
      int from = (i * LEDS_PER_STRIP + ROWS / 2) / ROWS;
      int to = ((i + 1) * LEDS_PER_STRIP + ROWS / 2) / ROWS;
      int row = down ? i : ROWS - 1 - i;
      boxFirst[row][k] = from;
      boxCount[row][k] = to - from;
    }
  }
}

void addLeds() {
  for (auto *s : strips) {
    s->begin();
    s->clear();
    s->show();
  }
}

// ---------------------------------------------------------------------
//  Funkpaket Chef -> alle:  "LD", Version, Helligkeit, Bild[ROWS][COLS] (RGB)
// ---------------------------------------------------------------------
struct __attribute__((packed)) FramePacket {
  char magic[2];
  uint8_t version;
  uint8_t brightness;
  uint8_t frame[ROWS][COLS][3];  // RGB pro Kasten
};
static_assert(sizeof(FramePacket) <= 250, "ESP-NOW-Paket zu groß");
static_assert(sizeof(CRGB) == 3, "CRGB muss 3 Bytes groß sein");

CRGB frame[ROWS][COLS];        // aktuelles Bild der ganzen Decke
uint8_t shownBrightness = 0;
const uint8_t ESPNOW_BROADCAST_ADDR[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void showMyRow() {
  for (int k = 0; k < NUM_STRIPS; k++) {
    strips[k]->setBrightness(shownBrightness);
    for (int r = 0; r < ROWS; r++) {
      CRGB c = frame[r][MY_COL];
      uint8_t w = 0;
#if USE_WHITE_CHANNEL
      // Der gemeinsame Weiß-Anteil geht auf die weiße LED (heller, schöneres Weiß)
      w = min(c.r, min(c.g, c.b));
      c.r -= w; c.g -= w; c.b -= w;
#endif
      uint32_t color = strips[k]->Color(c.r, c.g, c.b, w);
      for (int i = 0; i < boxCount[r][k]; i++) strips[k]->setPixelColor(boxFirst[r][k] + i, color);
    }
    strips[k]->show();
  }
}

// Mit WLAN: alle ESP32 funken auf dem Kanal des WLANs, und Updates gehen
// über WLAN (Arduino IDE oder Browser http://IP/update). Ist das WLAN beim
// Start nicht da, wird alle 10 s neu versucht – Updates gehen dann, sobald es
// da ist. Ohne eingetragenes WLAN: fester Kanal ESPNOW_CHANNEL.
char hostName[32];
WebServer web(80);
HTTPUpdateServer webUpdater;
bool otaStarted = false;
unsigned long lastWifiTry = 0;

void startRadio() {
  snprintf(hostName, sizeof(hostName), "%s%d", OTA_HOSTNAME_PREFIX, ROW_NUMBER);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName);
  if (strlen(WIFI_SSID) > 0) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Verbinde mit WLAN \"%s\"", WIFI_SSID);
    for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
      delay(250);
      Serial.print('.');
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? " verbunden" : " noch nicht da, versuche weiter");
  } else {
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    Serial.printf("Ohne WLAN, Funkkanal %d (kein Update über WLAN)\n", ESPNOW_CHANNEL);
  }
  if (esp_now_init() != ESP_OK) Serial.println("ESP-NOW konnte nicht starten!");
}

// In jeder loop() aufrufen: hält WLAN und die Update-Wege am Leben.
void handleNetwork() {
  if (strlen(WIFI_SSID) == 0) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 10000) {
      lastWifiTry = millis();
      WiFi.reconnect();
    }
    return;
  }
  if (!otaStarted) {
    otaStarted = true;
    ArduinoOTA.setHostname(hostName);
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([] { Serial.println("Update startet ..."); });
    ArduinoOTA.begin();
    webUpdater.setup(&web);  // Notfall-Update im Browser: http://IP/update
    web.on("/", [] {
      web.send(200, "text/plain", String("LED-Decke Reihe ") + ROW_NUMBER + " laeuft");
    });
    web.begin();
    Serial.printf("WLAN ok: http://%s  (Kanal %d), Update-Name \"%s\"\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel(), hostName);
  }
  ArduinoOTA.handle();
  web.handleClient();
}

#if !IS_MASTER
// ---------------------------------------------------------------------
//  Reihen-ESP32: Bilder empfangen und eigene Reihe anzeigen
// ---------------------------------------------------------------------
FramePacket rxPacket;
volatile bool rxNew = false;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
unsigned long lastRx = 0;

void onReceive(const uint8_t *data, int len) {
  if (len != sizeof(FramePacket) || data[0] != 'L' || data[1] != 'D' || data[2] != 1) return;
  portENTER_CRITICAL(&rxMux);
  memcpy(&rxPacket, data, sizeof(FramePacket));
  rxNew = true;
  portEXIT_CRITICAL(&rxMux);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onReceiveCb(const esp_now_recv_info_t *info, const uint8_t *data, int len) { onReceive(data, len); }
#else
void onReceiveCb(const uint8_t *mac, const uint8_t *data, int len) { onReceive(data, len); }
#endif

void setup() {
  Serial.begin(115200);
  buildMap();
  addLeds();
  startRadio();
  esp_now_register_recv_cb(onReceiveCb);
  Serial.printf("\nLED-Decke Reihe %d: 2x %d LEDs an GPIO%d/%d, warte auf Chef (Reihe %d)\n",
                ROW_NUMBER, LEDS_PER_STRIP, STRIP_A_PIN, STRIP_B_PIN, MASTER_ROW);
}

void loop() {
  handleNetwork();
  if (rxNew) {
    portENTER_CRITICAL(&rxMux);
    memcpy(frame, rxPacket.frame, sizeof(frame));
    shownBrightness = rxPacket.brightness;
    rxNew = false;
    portEXIT_CRITICAL(&rxMux);
    lastRx = millis();
    showMyRow();
  }
  // Ohne Funk bleibt einfach das letzte Bild stehen.
  delay(1);
}

#else
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

void loadState() {
  prefs.begin("leddecke", false);
  if (prefs.getBytesLength("boxes") == sizeof(boxes)) prefs.getBytes("boxes", boxes, sizeof(boxes));
  if (prefs.getBytesLength("scenes") == sizeof(scenes)) prefs.getBytes("scenes", scenes, sizeof(scenes));
  brightness = constrain(prefs.getInt("bri", brightness), 0, MAX_BRIGHTNESS);
  speed = constrain(prefs.getFloat("spd", 1.0), 0.1f, 5.0f);
  fxColor = CRGB(prefs.getUInt("col", 0xff6a00));
  mode = prefs.getInt("mode", -1);
  if (mode < -1 || mode >= NUM_EFFECTS || mode == WIRING_TEST) mode = -1;
}

void saveState() {
  prefs.putBytes("boxes", boxes, sizeof(boxes));
  prefs.putInt("bri", brightness);
  prefs.putFloat("spd", speed);
  prefs.putUInt("col", ((uint32_t)fxColor.r << 16) | (fxColor.g << 8) | fxColor.b);
  prefs.putInt("mode", mode);
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
class StateCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    static uint8_t buf[10 + ROWS * COLS * 6];
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
    c->setValue(buf, sizeof(buf));
  }
};

class ScenesCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    Lock l;
    c->setValue(sceneNamesJson());
  }
};

// Befehle als JSON: {"op":"boxes"|"fill"|"settings"|"scene_save"|"scene_load"|"scene_delete", ...}
class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    JsonDocument doc;
    NimBLEAttValue v = c->getValue();
    if (deserializeJson(doc, (const char *)v.data(), v.length())) return;
    String op = doc["op"] | "";
    if (op == "boxes") cmdBoxes(doc);
    else if (op == "fill") cmdFill(doc);
    else if (op == "settings") cmdSettings(doc);
    else if (op == "scene_save") cmdScene('s', doc["name"] | "");
    else if (op == "scene_load") cmdScene('l', doc["name"] | "");
    else if (op == "scene_delete") cmdScene('d', doc["name"] | "");
  }
};

void startBle() {
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setMTU(517);
  NimBLEServer *srv = NimBLEDevice::createServer();
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
//  Chef: Hauptschleife
// ---------------------------------------------------------------------
FramePacket txPacket;

void setup() {
  Serial.begin(115200);
  mtx = xSemaphoreCreateMutex();
  buildMap();
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) sparkStart[r][c] = -10;
  addLeds();
  loadState();
  startRadio();
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, ESPNOW_BROADCAST_ADDR, 6);
  peer.channel = 0;  // aktueller Kanal
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  startBle();
  Serial.printf("\nLED-Decke CHEF (Reihe %d): 2x %d LEDs an GPIO%d/%d\n",
                ROW_NUMBER, LEDS_PER_STRIP, STRIP_A_PIN, STRIP_B_PIN);
}

void loop() {
  static unsigned long lastFrame = 0;
  handleNetwork();
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
    // Erst an alle schicken, dann selbst anzeigen – so leuchten alle gleichzeitig.
    txPacket.magic[0] = 'L';
    txPacket.magic[1] = 'D';
    txPacket.version = 1;
    txPacket.brightness = shownBrightness;
    memcpy(txPacket.frame, frame, sizeof(frame));
    esp_now_send(ESPNOW_BROADCAST_ADDR, (const uint8_t *)&txPacket, sizeof(txPacket));
    showMyRow();
  }
  delay(1);
}
#endif
