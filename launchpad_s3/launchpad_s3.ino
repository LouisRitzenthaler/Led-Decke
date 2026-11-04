// LED-Decke – Launchpad-Steuerung auf einem ESP32-S3 (ersetzt den Raspberry Pi)
//
// Das Launchpad MK2 steckt an der Buchse "USB" des ESP32-S3 (Lötpunkt USB-OTG zu).
// Strom kommt über die Buchse "COM" (USB-C-Ladegerät, mind. 1 A).
// Der ESP32-S3 verbindet sich per Bluetooth mit der Decke ("LED-Decke") und
// zeigt auf dem Launchpad, was die Decke gerade macht. Updates per WLAN (OTA).
//
// Belegung (Spalten 1-9 von links, Reihen a-i von oben, a = obere runde Tasten):
//   1-7 / a-i = die Kästen. Drücken: blinkt = ausgewählt, nochmal: abgewählt
//   Spalte 8  = Farben a Rot, b Orange, c Gelb, d Grün, e Türkis, f Blau, g Lila, h Pink, i Weiß
//   9b An, 9c Aus, 9d Hell+, 9e Hell-, 9f Regenbogen, 9g Farbwechsel, 9h Launchpad-Start, 9i Funkeln
//   Ohne Auswahl gilt alles für die ganze Decke.
//
// Arduino IDE, Menü Tools:
//   Board: "ESP32S3 Dev Module"
//   USB Mode: "Hardware CDC and JTAG"    USB CDC On Boot: "Disabled"
//   Flash Size: "16MB (128Mb)"  Flash Mode: "QIO 80MHz"  PSRAM: "Disabled"
//   Partition Scheme: "16M Flash (3MB APP/9.9MB FATFS)"
// Bibliothek: NimBLE-Arduino (wie bei der Decke)

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <NimBLEDevice.h>

// ======================= EINSTELLUNGEN =======================
#define WIFI_SSID      ""   // hier dein WLAN eintragen
#define WIFI_PASSWORD  ""
#define HOSTNAME       "launchpad-s3"   // Name für Updates (Port in der Arduino IDE)
#define BLE_NAME       "LED-Decke"      // Bluetooth-Name der Decke
// =============================================================

#define BLE_SERVICE "6e1d0001-8a3c-4c5b-9b1e-1ed0c0dec0de"
#define BLE_CMD     "6e1d0002-8a3c-4c5b-9b1e-1ed0c0dec0de"
#define BLE_STATE   "6e1d0003-8a3c-4c5b-9b1e-1ed0c0dec0de"

const int ROWS = 9, COLS = 7;
const int STATE_SIZE = 10 + ROWS * COLS * 6;
const int BRIGHTNESS_STEP = 16;
const uint32_t POLL_MS = 100;          // so oft den Zustand der Decke holen
const uint32_t DESELECT_AFTER = 2500;  // Auswahl endet so lange nach Helligkeit

struct RGB {
  uint8_t r, g, b;
  bool operator==(const RGB &o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const RGB &o) const { return !(*this == o); }
  uint8_t top() const { return max(r, max(g, b)); }
};
const RGB BLACK = {0, 0, 0}, WHITE = {255, 255, 255};

// Spalte 8, von a bis i
const RGB COLORS[9] = {{255, 0, 0}, {255, 90, 0}, {255, 210, 0}, {0, 255, 0}, {0, 255, 200},
                       {0, 0, 255}, {140, 0, 255}, {255, 0, 140}, {255, 255, 255}};
// Spalte 9, von f bis i
const char *FX_NAMES[4] = {"rainbow", "colorfade", "launch", "sparkle"};
const RGB FX_COLORS[4] = {{255, 0, 255}, {0, 255, 255}, {255, 120, 0}, {255, 255, 255}};
// Reihenfolge der Effekte in der Decke (Nummer im Zustand)
const char *ESP_EFFECTS[] = {"rainbow", "colorfade", "wave", "breathe", "sparkle", "rain", "wiring_test",
                             "music_pulse", "music_vu", "music_beat", "music_switch", "launch"};
const int NUM_ESP_EFFECTS = sizeof(ESP_EFFECTS) / sizeof(ESP_EFFECTS[0]);

// ---------------------------------------------------------------------
//  Zustand der Decke (kommt per Bluetooth)
// ---------------------------------------------------------------------
struct Deck {
  int mode = -1, brightness = 80, maxBrightness = 160;
  RGB boxes[ROWS][COLS];
  RGB frame[ROWS][COLS];
};
Deck deck;                       // nur mit deckLock benutzen
bool haveState = false;
uint32_t lastOk = 0;             // millis() der letzten Antwort der Decke
bool everOnline = false;
SemaphoreHandle_t deckLock;
QueueHandle_t cmdQueue;          // Befehle an die Decke (JSON-Text)
volatile bool otaRunning = false;

struct Cmd { char text[480]; };

void post(const char *json) {
  Cmd c;
  strlcpy(c.text, json, sizeof(c.text));
  xQueueSend(cmdQueue, &c, pdMS_TO_TICKS(200));
}

bool synced() { return haveState && millis() - lastOk < 3000; }

bool parseState(const uint8_t *d, size_t n) {
  if (n != (size_t)STATE_SIZE || d[0] != 1 || d[8] != ROWS || d[9] != COLS) return false;
  xSemaphoreTake(deckLock, portMAX_DELAY);
  deck.mode = (int8_t)d[1];
  deck.brightness = d[2];
  deck.maxBrightness = max(1, (int)d[3]);
  memcpy(deck.boxes, d + 10, ROWS * COLS * 3);
  memcpy(deck.frame, d + 10 + ROWS * COLS * 3, ROWS * COLS * 3);
  haveState = true;
  lastOk = millis();
  everOnline = true;
  xSemaphoreGive(deckLock);
  return true;
}

// Bluetooth-Suche: hört sofort auf, sobald die Decke gefunden ist
const NimBLEAdvertisedDevice *volatile foundDeck = nullptr;
class DeckScan : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *d) override {
    if (foundDeck) return;
    if (d->isAdvertisingService(NimBLEUUID(BLE_SERVICE)) || d->getName() == BLE_NAME) {
      foundDeck = d;
      NimBLEDevice::getScan()->stop();
    }
  }
};

// Läuft für sich: sucht die Decke, verbindet, schickt Befehle, holt den Zustand.
void deckTask(void *) {
  NimBLEUUID svcUuid(BLE_SERVICE);
  for (;;) {
    if (otaRunning) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(200);   // nur 25 % der Zeit suchen, sonst kommt das WLAN nicht durch
    scan->setWindow(50);
    static DeckScan callbacks;
    scan->setScanCallbacks(&callbacks);
    foundDeck = nullptr;
    scan->clearResults();
    scan->start(4000, false, true);
    uint32_t t0 = millis();
    while (!foundDeck && scan->isScanning() && millis() - t0 < 4500) vTaskDelay(pdMS_TO_TICKS(20));
    if (scan->isScanning()) scan->stop();
    const NimBLEAdvertisedDevice *dev = foundDeck;
    if (!dev) {
      Serial.println("Decke nicht gefunden, suche weiter");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    NimBLEClient *client = NimBLEDevice::createClient();
    client->setConnectTimeout(5000);
    if (client->connect(dev)) {
      NimBLERemoteService *svc = client->getService(svcUuid);
      NimBLERemoteCharacteristic *cmd = svc ? svc->getCharacteristic(BLE_CMD) : nullptr;
      NimBLERemoteCharacteristic *state = svc ? svc->getCharacteristic(BLE_STATE) : nullptr;
      if (cmd && state) {
        Serial.println("Decke per Bluetooth verbunden");
        xQueueReset(cmdQueue);           // alte Tasten von vor der Verbindung vergessen
        Cmd c;
        bool havePending = false;
        uint32_t lastRead = 0;
        while (client->isConnected() && !otaRunning) {
          if (!havePending) havePending = xQueueReceive(cmdQueue, &c, 0) == pdTRUE;
          if (havePending) {             // bleibt gemerkt, bis er sicher angekommen ist
            if (cmd->writeValue((const uint8_t *)c.text, strlen(c.text), true)) havePending = false;
            else vTaskDelay(pdMS_TO_TICKS(50));
            continue;
          }
          if (millis() - lastRead >= POLL_MS || lastRead == 0) {
            lastRead = millis();
            NimBLEAttValue v = state->readValue();
            parseState(v.data(), v.length());
          } else {
            vTaskDelay(pdMS_TO_TICKS(5));
          }
        }
      }
      client->disconnect();
    }
    NimBLEDevice::deleteClient(client);
    Serial.println("Bluetooth-Verbindung weg, verbinde neu");
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ---------------------------------------------------------------------
//  Launchpad MK2 per USB (MIDI)
//  Eigene USB-Anmeldung, so wie sie ein PC macht: zuerst 64 Byte der
//  Gerätebeschreibung anfragen, dann noch mal zurücksetzen, dann Adresse.
//  Benutzt direkt den USB-Treiber (HCD) aus dem ESP32-Paket.
// ---------------------------------------------------------------------
#include <sys/queue.h>
#include "esp_private/usb_phy.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"

extern "C" {
// --- Auszug aus esp-idf v5.5 components/usb/private_include/hcd.h + usb_private.h ---
typedef enum { HCD_PORT_STATE_NOT_POWERED, HCD_PORT_STATE_DISCONNECTED, HCD_PORT_STATE_DISABLED, HCD_PORT_STATE_RESETTING,
               HCD_PORT_STATE_SUSPENDED, HCD_PORT_STATE_RESUMING, HCD_PORT_STATE_ENABLED, HCD_PORT_STATE_RECOVERY } hcd_port_state_t;
typedef enum { HCD_PIPE_STATE_ACTIVE, HCD_PIPE_STATE_HALTED } hcd_pipe_state_t;
typedef enum { HCD_PORT_EVENT_NONE, HCD_PORT_EVENT_CONNECTION, HCD_PORT_EVENT_DISCONNECTION, HCD_PORT_EVENT_ERROR,
               HCD_PORT_EVENT_OVERCURRENT } hcd_port_event_t;
typedef enum { HCD_PIPE_EVENT_NONE, HCD_PIPE_EVENT_URB_DONE, HCD_PIPE_EVENT_ERROR_XFER, HCD_PIPE_EVENT_ERROR_URB_NOT_AVAIL,
               HCD_PIPE_EVENT_ERROR_OVERFLOW, HCD_PIPE_EVENT_ERROR_STALL } hcd_pipe_event_t;
typedef enum { HCD_PORT_CMD_POWER_ON, HCD_PORT_CMD_POWER_OFF, HCD_PORT_CMD_RESET, HCD_PORT_CMD_SUSPEND, HCD_PORT_CMD_RESUME,
               HCD_PORT_CMD_DISABLE } hcd_port_cmd_t;
typedef enum { HCD_PIPE_CMD_HALT, HCD_PIPE_CMD_FLUSH, HCD_PIPE_CMD_CLEAR } hcd_pipe_cmd_t;
typedef void *hcd_port_handle_t;
typedef void *hcd_pipe_handle_t;
typedef bool (*hcd_port_callback_t)(hcd_port_handle_t, hcd_port_event_t, void *, bool);
typedef bool (*hcd_pipe_callback_t)(hcd_pipe_handle_t, hcd_pipe_event_t, void *, bool);
typedef struct { uint32_t nptx_fifo_lines, ptx_fifo_lines, rx_fifo_lines; } hcd_fifo_settings_t;
typedef struct { int intr_flags; const hcd_fifo_settings_t *fifo_config; unsigned peripheral_map; } hcd_config_t;
typedef struct { hcd_port_callback_t callback; void *callback_arg; void *context; unsigned peripheral_idx; } hcd_port_config_t;
typedef struct { hcd_pipe_callback_t callback; void *callback_arg; void *context; const usb_ep_desc_t *ep_desc;
                 usb_speed_t dev_speed; uint8_t dev_addr; } hcd_pipe_config_t;
struct urb_s {
  TAILQ_ENTRY(urb_s) tailq_entry;
  void *hcd_ptr;
  uint32_t hcd_var;
  void *usb_host_client;
  bool usb_host_inflight;
  usb_transfer_t transfer;
};
typedef struct urb_s urb_t;
esp_err_t hcd_install(const hcd_config_t *config);
esp_err_t hcd_port_init(int port_number, const hcd_port_config_t *port_config, hcd_port_handle_t *port_hdl);
esp_err_t hcd_port_command(hcd_port_handle_t port_hdl, hcd_port_cmd_t command);
hcd_port_state_t hcd_port_get_state(hcd_port_handle_t port_hdl);
esp_err_t hcd_port_get_speed(hcd_port_handle_t port_hdl, usb_speed_t *speed);
hcd_port_event_t hcd_port_handle_event(hcd_port_handle_t port_hdl);
esp_err_t hcd_port_recover(hcd_port_handle_t port_hdl);
esp_err_t hcd_pipe_alloc(hcd_port_handle_t port_hdl, const hcd_pipe_config_t *pipe_config, hcd_pipe_handle_t *pipe_hdl);
int hcd_pipe_get_mps(hcd_pipe_handle_t pipe_hdl);
esp_err_t hcd_pipe_free(hcd_pipe_handle_t pipe_hdl);
esp_err_t hcd_pipe_update_mps(hcd_pipe_handle_t pipe_hdl, int mps);
esp_err_t hcd_pipe_update_dev_addr(hcd_pipe_handle_t pipe_hdl, uint8_t dev_addr);
esp_err_t hcd_pipe_command(hcd_pipe_handle_t pipe_hdl, hcd_pipe_cmd_t command);
esp_err_t hcd_urb_enqueue(hcd_pipe_handle_t pipe_hdl, urb_t *urb);
urb_t *hcd_urb_dequeue(hcd_pipe_handle_t pipe_hdl);
urb_t *urb_alloc(size_t data_buffer_size, int num_isoc_packets);
void urb_free(urb_t *urb);
}

QueueHandle_t padQueue;          // gedrückte Tasten (Index wie led_index)
QueueHandle_t sysexQueue;        // fertige USB-MIDI-Pakete ans Launchpad
QueueHandle_t usbEvents;         // Meldungen vom USB-Treiber
volatile bool lpReady = false, lpGone = false;
struct UsbMsg { uint8_t len; uint8_t data[60]; };
struct UsbEv { uint8_t kind; void *h; int ev; };   // kind 0 = Anschluss, 1 = Pipe

hcd_port_handle_t usbPort = nullptr;
hcd_pipe_handle_t ep0 = nullptr, pipeIn = nullptr, pipeOut = nullptr;
urb_t *ctrlUrb = nullptr, *inUrb = nullptr, *outUrb = nullptr;
usb_ep_desc_t epInDesc, epOutDesc;
uint8_t cfgBuf[512];

static bool portCb(hcd_port_handle_t h, hcd_port_event_t e, void *, bool isr) {
  UsbEv x = {0, h, (int)e};
  BaseType_t w = pdFALSE;
  if (isr) xQueueSendFromISR(usbEvents, &x, &w); else xQueueSend(usbEvents, &x, 0);
  return w == pdTRUE;
}
static bool pipeCb(hcd_pipe_handle_t h, hcd_pipe_event_t e, void *, bool isr) {
  UsbEv x = {1, h, (int)e};
  BaseType_t w = pdFALSE;
  if (isr) xQueueSendFromISR(usbEvents, &x, &w); else xQueueSend(usbEvents, &x, 0);
  return w == pdTRUE;
}

bool portLost = false;
void portEvent() {
  hcd_port_event_t pe = hcd_port_handle_event(usbPort);
  if (pe == HCD_PORT_EVENT_DISCONNECTION || pe == HCD_PORT_EVENT_ERROR || pe == HCD_PORT_EVENT_OVERCURRENT) portLost = true;
}

// Auf eine Meldung für diese Pipe warten (Anschluss-Meldungen nebenbei erledigen)
int waitPipe(hcd_pipe_handle_t p, uint32_t ms) {
  uint32_t t0 = millis();
  UsbEv e;
  while (millis() - t0 < ms) {
    if (xQueueReceive(usbEvents, &e, pdMS_TO_TICKS(10)) != pdTRUE) continue;
    if (e.kind == 0) { portEvent(); if (portLost) return -1; }
    else if (e.h == p) return e.ev;
  }
  return 0;  // keine Antwort
}

void pipeRecover(hcd_pipe_handle_t p) {
  hcd_pipe_command(p, HCD_PIPE_CMD_HALT);
  hcd_pipe_command(p, HCD_PIPE_CMD_FLUSH);
  while (hcd_urb_dequeue(p)) {}
  hcd_pipe_command(p, HCD_PIPE_CMD_CLEAR);
}

const char *evName(int ev) {
  switch (ev) {
    case -1: return "Gerät weg";
    case 0: return "keine Antwort";
    case HCD_PIPE_EVENT_ERROR_XFER: return "Übertragungsfehler";
    case HCD_PIPE_EVENT_ERROR_OVERFLOW: return "zu viele Daten";
    case HCD_PIPE_EVENT_ERROR_STALL: return "abgelehnt (STALL)";
    default: return "Fehler";
  }
}

// Steuer-Anfrage an Endpunkt 0. Gibt die Anzahl empfangener Daten-Bytes zurück, < 0 bei Fehler.
int ctrl(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, uint16_t len, uint8_t **data) {
  usb_setup_packet_t *s = (usb_setup_packet_t *)ctrlUrb->transfer.data_buffer;
  s->bmRequestType = reqType;
  s->bRequest = req;
  s->wValue = value;
  s->wIndex = index;
  s->wLength = len;
  int mps = hcd_pipe_get_mps(ep0);
  ctrlUrb->transfer.num_bytes = sizeof(usb_setup_packet_t) + ((reqType & 0x80) ? ((len + mps - 1) / mps) * mps : len);
  ctrlUrb->transfer.actual_num_bytes = 0;
  if (hcd_urb_enqueue(ep0, ctrlUrb) != ESP_OK) { Serial.println("USB: Anfrage nicht möglich"); return -1; }
  int ev = waitPipe(ep0, 1000);
  if (ev != HCD_PIPE_EVENT_URB_DONE) {
    Serial.printf("USB: Anfrage %02X/%02X: %s\n", reqType, req, evName(ev));
    pipeRecover(ep0);
    return -2;
  }
  urb_t *u = hcd_urb_dequeue(ep0);
  if (!u || u->transfer.status != USB_TRANSFER_STATUS_COMPLETED) {
    Serial.printf("USB: Anfrage %02X/%02X: Status %d\n", reqType, req, u ? (int)u->transfer.status : -9);
    return -3;
  }
  if (data) *data = u->transfer.data_buffer + sizeof(usb_setup_packet_t);
  return u->transfer.actual_num_bytes - sizeof(usb_setup_packet_t);
}

void freePipes() {
  if (pipeIn) { pipeRecover(pipeIn); hcd_pipe_free(pipeIn); pipeIn = nullptr; }
  if (pipeOut) { pipeRecover(pipeOut); hcd_pipe_free(pipeOut); pipeOut = nullptr; }
  if (ep0) { hcd_pipe_free(ep0); ep0 = nullptr; }
}

bool resetPort() {
  esp_err_t e = hcd_port_command(usbPort, HCD_PORT_CMD_RESET);
  if (e != ESP_OK) { Serial.printf("USB: Zurücksetzen fehlgeschlagen (%s)\n", esp_err_to_name(e)); return false; }
  vTaskDelay(pdMS_TO_TICKS(50));
  return true;
}

// Anmeldung wie am PC
bool enumerate() {
  portLost = false;
  vTaskDelay(pdMS_TO_TICKS(200));
  if (!resetPort()) return false;
  usb_speed_t sp = USB_SPEED_FULL;
  hcd_port_get_speed(usbPort, &sp);
  Serial.printf("USB: Gerät angeschlossen (%s Speed)\n", sp == USB_SPEED_LOW ? "Low" : sp == USB_SPEED_FULL ? "Full" : "High");
  hcd_pipe_config_t pc = {pipeCb, nullptr, nullptr, nullptr, sp, 0};
  if (hcd_pipe_alloc(usbPort, &pc, &ep0) != ESP_OK) { Serial.println("USB: Pipe fehlgeschlagen"); return false; }

  // 1. Gerätebeschreibung mit 64 Byte anfragen (wie Windows und Linux)
  uint8_t *d = nullptr;
  int n = -1;
  for (int i = 0; i < 3 && n < 8; i++) {
    n = ctrl(0x80, 0x06, 0x0100, 0, 64, &d);
    Serial.printf("USB: Gerätebeschreibung (64 Byte angefragt): %d Byte erhalten\n", n);
    if (n < 8) vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (n < 8) return false;
  uint8_t mps0 = d[7];
  if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64) mps0 = 64;

  // 2. Noch einmal zurücksetzen (macht der PC auch), dann Adresse vergeben
  if (!resetPort()) return false;
  if (ctrl(0x00, 0x05, 1, 0, 0, nullptr) < 0) return false;
  vTaskDelay(pdMS_TO_TICKS(20));
  hcd_pipe_update_dev_addr(ep0, 1);
  hcd_pipe_update_mps(ep0, mps0);

  // 3. Ganze Gerätebeschreibung
  n = ctrl(0x80, 0x06, 0x0100, 0, 18, &d);
  if (n < 18) return false;
  Serial.printf("USB-Gerät gefunden: %04X:%04X\n", d[8] | (d[9] << 8), d[10] | (d[11] << 8));

  // 4. Konfiguration lesen
  n = ctrl(0x80, 0x06, 0x0200, 0, 9, &d);
  if (n < 9) return false;
  int total = d[2] | (d[3] << 8);
  uint8_t cfgValue = d[5];
  if (total > (int)sizeof(cfgBuf)) total = sizeof(cfgBuf);
  n = ctrl(0x80, 0x06, 0x0200, 0, total, &d);
  if (n < 9) return false;
  memcpy(cfgBuf, d, n);

  // MIDI-Anschluss suchen (Audio-Klasse 1, MIDI-Streaming 3, Bulk-Endpunkte)
  const uint8_t *p = cfgBuf, *end = cfgBuf + n;
  int iface = -1, alt = 0;
  bool inMidi = false, haveIn = false, haveOut = false;
  while (p < end && p[0] > 0) {
    if (p[1] == 0x04) {
      inMidi = p[5] == 1 && p[6] == 3 && iface < 0;
      if (inMidi) { iface = p[2]; alt = p[3]; }
    } else if (p[1] == 0x05 && inMidi && (p[3] & 0x03) == 2) {
      if (p[2] & 0x80) { memcpy(&epInDesc, p, 7); haveIn = true; }
      else { memcpy(&epOutDesc, p, 7); haveOut = true; }
    }
    p += p[0];
  }
  if (iface < 0 || !haveIn || !haveOut) { Serial.println("USB-Gerät ist kein Launchpad (kein MIDI)"); return false; }

  // 5. Konfiguration einschalten
  if (ctrl(0x00, 0x09, cfgValue, 0, 0, nullptr) < 0) return false;
  if (alt) ctrl(0x01, 0x0B, alt, iface, 0, nullptr);

  // 6. MIDI-Pipes anlegen
  hcd_pipe_config_t pin = {pipeCb, nullptr, nullptr, &epInDesc, sp, 1};
  hcd_pipe_config_t pout = {pipeCb, nullptr, nullptr, &epOutDesc, sp, 1};
  if (hcd_pipe_alloc(usbPort, &pin, &pipeIn) != ESP_OK || hcd_pipe_alloc(usbPort, &pout, &pipeOut) != ESP_OK) {
    Serial.println("USB: MIDI-Pipes fehlgeschlagen");
    return false;
  }
  int mpsIn = USB_EP_DESC_GET_MPS(&epInDesc);
  if (!inUrb) inUrb = urb_alloc(64, 0);
  if (!outUrb) outUrb = urb_alloc(64, 0);
  inUrb->transfer.num_bytes = mpsIn > 64 ? 64 : mpsIn;
  hcd_urb_enqueue(pipeIn, inUrb);
  return true;
}

void handleIn(int ev) {
  urb_t *u = hcd_urb_dequeue(pipeIn);
  if (ev != HCD_PIPE_EVENT_URB_DONE) { pipeRecover(pipeIn); u = inUrb; }
  else if (u && u->transfer.status == USB_TRANSFER_STATUS_COMPLETED) {
    for (int i = 0; i + 3 < u->transfer.actual_num_bytes; i += 4) {
      const uint8_t *b = u->transfer.data_buffer + i;
      uint8_t cin = b[0] & 0x0F;
      // Note an (Raster, rechte Spalte) oder Controller (obere runde Tasten)
      if ((cin == 0x9 || cin == 0xB) && b[3] > 0) xQueueSend(padQueue, &b[2], 0);
    }
  }
  inUrb->transfer.actual_num_bytes = 0;
  hcd_urb_enqueue(pipeIn, inUrb);
}

void usbTask(void *) {
  usb_phy_config_t phy = {};
  phy.controller = USB_PHY_CTRL_OTG;
  phy.target = USB_PHY_TARGET_INT;
  phy.otg_mode = USB_OTG_MODE_HOST;
  phy.otg_speed = USB_PHY_SPEED_UNDEFINED;
  usb_phy_handle_t phyHandle;
  esp_err_t e = usb_new_phy(&phy, &phyHandle);
  hcd_config_t hc = {ESP_INTR_FLAG_LEVEL1, nullptr, 1};
  if (e == ESP_OK) e = hcd_install(&hc);
  hcd_port_config_t pc = {portCb, nullptr, nullptr, 0};
  if (e == ESP_OK) e = hcd_port_init(0, &pc, &usbPort);
  if (e == ESP_OK) e = hcd_port_command(usbPort, HCD_PORT_CMD_POWER_ON);
  Serial.printf("USB-Host (eigene Anmeldung): %s\n", esp_err_to_name(e));
  if (e != ESP_OK) vTaskDelete(nullptr);
  ctrlUrb = urb_alloc(sizeof(usb_setup_packet_t) + sizeof(cfgBuf), 0);

  int tries = 0;
  for (;;) {
    // Warten, bis ein Gerät steckt
    while (hcd_port_get_state(usbPort) != HCD_PORT_STATE_DISABLED && hcd_port_get_state(usbPort) != HCD_PORT_STATE_ENABLED) {
      UsbEv ev;
      if (xQueueReceive(usbEvents, &ev, pdMS_TO_TICKS(100)) == pdTRUE && ev.kind == 0) portEvent();
      if (hcd_port_get_state(usbPort) == HCD_PORT_STATE_RECOVERY) {
        hcd_port_recover(usbPort);
        hcd_port_command(usbPort, HCD_PORT_CMD_POWER_ON);
      }
    }
    if (enumerate()) break;
    freePipes();
    Serial.printf("Launchpad antwortet nicht, neuer Versuch (%d)\n", ++tries);
    // Anschluss kurz ausschalten und wieder an
    hcd_port_command(usbPort, HCD_PORT_CMD_POWER_OFF);
    vTaskDelay(pdMS_TO_TICKS(1000));
    UsbEv ev;
    while (xQueueReceive(usbEvents, &ev, 0) == pdTRUE) if (ev.kind == 0) portEvent();
    if (hcd_port_get_state(usbPort) == HCD_PORT_STATE_RECOVERY) hcd_port_recover(usbPort);
    hcd_port_command(usbPort, HCD_PORT_CMD_POWER_ON);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  lpReady = true;
  Serial.println("Launchpad verbunden");

  bool outBusy = false;
  for (;;) {
    UsbEv ev;
    if (xQueueReceive(usbEvents, &ev, pdMS_TO_TICKS(5)) == pdTRUE) {
      if (ev.kind == 0) { portEvent(); if (portLost) lpGone = true; }
      else if (ev.h == pipeIn) handleIn(ev.ev);
      else if (ev.h == pipeOut) {
        if (ev.ev != HCD_PIPE_EVENT_URB_DONE) pipeRecover(pipeOut);
        else hcd_urb_dequeue(pipeOut);
        outBusy = false;
      }
    }
    UsbMsg m;
    if (!outBusy && !lpGone && xQueueReceive(sysexQueue, &m, 0) == pdTRUE) {
      memcpy(outUrb->transfer.data_buffer, m.data, m.len);
      outUrb->transfer.num_bytes = m.len;
      outUrb->transfer.actual_num_bytes = 0;
      outBusy = hcd_urb_enqueue(pipeOut, outUrb) == ESP_OK;
    }
  }
}

// MIDI-SysEx in USB-Pakete (je 4 Byte) verpacken und schicken
void sendSysex(const uint8_t *d, int n) {
  if (!lpReady) return;
  UsbMsg m;
  int k = 0;
  for (int i = 0; i < n && k + 4 <= (int)sizeof(m.data); i += 3) {
    int left = n - i;
    uint8_t cin = left > 3 ? 0x4 : (left == 3 ? 0x7 : (left == 2 ? 0x6 : 0x5));
    m.data[k++] = cin;
    for (int j = 0; j < 3; j++) m.data[k++] = j < left ? d[i + j] : 0;
  }
  m.len = k;
  xQueueSend(sysexQueue, &m, pdMS_TO_TICKS(100));
}

void lpCommand(const uint8_t *data, int n) {  // F0 00 20 29 02 18 <data> F7
  uint8_t buf[64] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x18};
  memcpy(buf + 6, data, n);
  buf[6 + n] = 0xF7;
  sendSysex(buf, 7 + n);
}

// x = Spalte 1..9 von links, y = Reihe 1..9 von unten (9 = obere runde Tasten)
int ledIndex(int x, int y) { return y == 9 ? 104 + x - 1 : y * 10 + x; }

RGB shown[128];
bool shownValid[128];

// Nur geänderte Tasten schicken, je 8 in einem Paket (passt in ein USB-Paket)
void showLeds(const RGB *leds, const bool *use) {
  uint8_t data[1 + 8 * 4];
  int n = 0, count = 0;
  for (int i = 0; i < 128; i++) {
    if (!use[i] || (shownValid[i] && shown[i] == leds[i])) continue;
    shown[i] = leds[i];
    shownValid[i] = true;
    if (count == 0) data[n++] = 0x0B;
    data[n++] = i;
    data[n++] = leds[i].r * 63 / 255;
    data[n++] = leds[i].g * 63 / 255;
    data[n++] = leds[i].b * 63 / 255;
    if (++count == 8) { lpCommand(data, n); n = count = 0; }
  }
  if (count) lpCommand(data, n);
}

void startUsb() {
  padQueue = xQueueCreate(32, 1);
  sysexQueue = xQueueCreate(24, sizeof(UsbMsg));
  usbEvents = xQueueCreate(32, sizeof(UsbEv));
  xTaskCreatePinnedToCore(usbTask, "usb", 6144, nullptr, 5, nullptr, 0);
}
// ---------------------------------------------------------------------
//  WLAN: nach dem Einschalten 2 Minuten suchen, verbunden bleiben,
//  bei Verlust 2 Minuten weitersuchen, sonst WLAN aus.
// ---------------------------------------------------------------------
bool wifiOff = false, otaStarted = false;
uint32_t wifiUntil = 120000, lastWifiTry = 0;

void startWifi() {
  if (!strlen(WIFI_SSID)) { wifiOff = true; return; }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void handleNetwork() {
  if (wifiOff) return;
  static wl_status_t lastStatus = WL_IDLE_STATUS;
  if (WiFi.status() != lastStatus) {
    lastStatus = WiFi.status();
    Serial.printf("WLAN-Status: %d (3 = verbunden, 1 = Hotspot nicht gefunden, 4/6 = Passwort/Verbindung falsch)\n", lastStatus);
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() > wifiUntil && !Update.isRunning()) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      wifiOff = true;
      Serial.println("Kein Hotspot - WLAN aus");
    } else if (millis() - lastWifiTry > 15000) {
      lastWifiTry = millis();
      WiFi.reconnect();
    }
    return;
  }
  wifiUntil = millis() + 120000;
  if (!otaStarted) {
    otaStarted = true;
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.onStart([] { otaRunning = true; });   // Bluetooth ruht während des Updates
    ArduinoOTA.onError([](ota_error_t) { otaRunning = false; });
    ArduinoOTA.begin();
    Serial.printf("WLAN ok: %s, Update-Name \"%s\"\n", WiFi.localIP().toString().c_str(), HOSTNAME);
  }
  ArduinoOTA.handle();
}

// ---------------------------------------------------------------------
//  Bedienung (wie vorher am Raspberry Pi)
// ---------------------------------------------------------------------
int8_t boxRow[128], boxCol[128], colorAt[128], funcAt[128];
enum { F_NONE = -1, F_ON = 10, F_OFF, F_BRI_UP, F_BRI_DOWN };  // 0..3 = Effekte
bool selected[ROWS][COLS];
RGB remembered[ROWS][COLS];      // Grundfarbe von Kästen, die per Helligkeit ganz aus sind
bool hasRemembered[ROWS][COLS];
RGB saved[ROWS][COLS];           // Kästen vor "Alles aus", für "Alles an"
bool hasSaved = false;
uint32_t deselectAt = 0;

bool anySelected() {
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (selected[r][c]) return true;
  return false;
}
void clearSelection() { memset(selected, 0, sizeof(selected)); }

void hexColor(RGB c, char *out) { sprintf(out, "#%02x%02x%02x", c.r, c.g, c.b); }

// Farbe -> Grundfarbe in voller Helligkeit + Helligkeit 0..1 (in Zehnteln)
float splitColor(RGB c, RGB &base) {
  uint8_t m = c.top();
  if (!m) { base = BLACK; return 0; }
  base = {(uint8_t)(c.r * 255 / m), (uint8_t)(c.g * 255 / m), (uint8_t)(c.b * 255 / m)};
  return roundf(m / 255.0f * 10) / 10;
}
RGB scaleColor(RGB c, float f) { return {(uint8_t)(c.r * f), (uint8_t)(c.g * f), (uint8_t)(c.b * f)}; }

// Kästen ändern: gleiche Farben in einem Befehl, ganze Decke mit "fill"
void apply(RGB changes[ROWS][COLS], bool use[ROWS][COLS]) {
  bool done[ROWS][COLS] = {};
  char hex[8], buf[480];
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      if (!use[r][c] || done[r][c]) continue;
      RGB col = changes[r][c];
      hexColor(col, hex);
      int total = 0;
      for (int r2 = 0; r2 < ROWS; r2++)
        for (int c2 = 0; c2 < COLS; c2++)
          if (use[r2][c2] && changes[r2][c2] == col) total++;
      if (total == ROWS * COLS) {
        snprintf(buf, sizeof(buf), "{\"op\":\"fill\",\"color\":\"%s\"}", hex);
        post(buf);
        memset(done, 1, sizeof(done));
        continue;
      }
      int n = 0, len = 0;
      for (int r2 = 0; r2 < ROWS; r2++)
        for (int c2 = 0; c2 < COLS; c2++) {
          if (!use[r2][c2] || done[r2][c2] || changes[r2][c2] != col) continue;
          done[r2][c2] = true;
          if (n == 0) len = snprintf(buf, sizeof(buf), "{\"op\":\"boxes\",\"color\":\"%s\",\"cells\":[", hex);
          len += snprintf(buf + len, sizeof(buf) - len, "%s[%d,%d]", n ? "," : "", r2, c2);
          if (++n == 20) {          // die Decke nimmt max. 512 Zeichen pro Befehl
            strcat(buf, "]}");
            post(buf);
            n = 0;
          }
        }
      if (n) { strcat(buf, "]}"); post(buf); }
    }
  // sofort merken, damit schnelles Drücken nicht verloren geht
  xSemaphoreTake(deckLock, portMAX_DELAY);
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (use[r][c]) deck.boxes[r][c] = changes[r][c];
  deck.mode = -1;
  xSemaphoreGive(deckLock);
}

Deck snapshot() {
  xSemaphoreTake(deckLock, portMAX_DELAY);
  Deck d = deck;
  xSemaphoreGive(deckLock);
  return d;
}

void onColor(RGB col) {
  char hex[8], buf[64];
  if (!anySelected()) {  // nichts ausgewählt: Farbe für Effekte wie Welle
    hexColor(col, hex);
    snprintf(buf, sizeof(buf), "{\"op\":\"settings\",\"color\":\"%s\"}", hex);
    post(buf);
    return;
  }
  Deck d = snapshot();
  static RGB ch[ROWS][COLS];
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (selected[r][c]) {
        RGB base;
        float level = splitColor(d.boxes[r][c], base);
        ch[r][c] = scaleColor(col, level > 0 ? level : 1.0f);
      }
  apply(ch, selected);
  clearSelection();
}

void setMode(const char *m, int index) {
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"op\":\"settings\",\"mode\":\"%s\"}", m);
  post(buf);
  xSemaphoreTake(deckLock, portMAX_DELAY);
  deck.mode = index;
  xSemaphoreGive(deckLock);
}

void onOff(bool on) {
  Deck d = snapshot();
  static RGB ch[ROWS][COLS];
  static bool use[ROWS][COLS];
  memset(use, 0, sizeof(use));
  if (anySelected()) {
    for (int r = 0; r < ROWS; r++)
      for (int c = 0; c < COLS; c++) {
        if (!selected[r][c]) continue;
        bool lit = d.boxes[r][c].top() > 0;
        if (on && !lit) {
          ch[r][c] = hasRemembered[r][c] ? remembered[r][c] : WHITE;
          hasRemembered[r][c] = false;
          use[r][c] = true;
        } else if (!on && lit) {
          remembered[r][c] = d.boxes[r][c];
          hasRemembered[r][c] = true;
          ch[r][c] = BLACK;
          use[r][c] = true;
        }
      }
    apply(ch, use);
    return;
  }
  bool anyLit = false;
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) anyLit |= d.boxes[r][c].top() > 0;
  if (!on) {  // ganze Decke aus, vorher merken
    if (anyLit) { memcpy(saved, d.boxes, sizeof(saved)); hasSaved = true; }
    for (int r = 0; r < ROWS; r++)
      for (int c = 0; c < COLS; c++) { ch[r][c] = BLACK; use[r][c] = true; }
  } else {    // ganze Decke wieder an (wie vorher, sonst weiß), Effekt aus
    if (anyLit) {
      if (d.mode >= 0) setMode("static", -1);
      return;
    }
    for (int r = 0; r < ROWS; r++)
      for (int c = 0; c < COLS; c++) { ch[r][c] = hasSaved ? saved[r][c] : WHITE; use[r][c] = true; }
  }
  apply(ch, use);
}

void onFunction(int f) {
  if (f == F_ON || f == F_OFF) {
    onOff(f == F_ON);
    clearSelection();
    deselectAt = 0;
  } else if (f == F_BRI_UP || f == F_BRI_DOWN) {
    int step = f == F_BRI_UP ? 1 : -1;
    Deck d = snapshot();
    if (!anySelected()) {  // ganze Decke heller/dunkler
      int bri = constrain(d.brightness + step * BRIGHTNESS_STEP, 0, d.maxBrightness);
      char buf[64];
      snprintf(buf, sizeof(buf), "{\"op\":\"settings\",\"brightness\":%d}", bri);
      post(buf);
      xSemaphoreTake(deckLock, portMAX_DELAY);
      deck.brightness = bri;
      xSemaphoreGive(deckLock);
      return;
    }
    static RGB ch[ROWS][COLS];
    for (int r = 0; r < ROWS; r++)
      for (int c = 0; c < COLS; c++) {
        if (!selected[r][c]) continue;
        RGB base;
        float level = splitColor(d.boxes[r][c], base);
        if (base == BLACK) base = hasRemembered[r][c] ? remembered[r][c] : WHITE;
        level = roundf(constrain(level + step * 0.1f, 0.0f, 1.0f) * 10) / 10;
        if (level == 0) { remembered[r][c] = base; hasRemembered[r][c] = true; }
        ch[r][c] = scaleColor(base, level);
      }
    apply(ch, selected);
    deselectAt = millis() + DESELECT_AFTER;
  } else if (f >= 0 && f < 4) {  // Effekt: an, oder aus wenn er schon läuft
    Deck d = snapshot();
    bool running = d.mode >= 0 && d.mode < NUM_ESP_EFFECTS && strcmp(ESP_EFFECTS[d.mode], FX_NAMES[f]) == 0;
    int index = -1;
    for (int i = 0; i < NUM_ESP_EFFECTS; i++)
      if (strcmp(ESP_EFFECTS[i], FX_NAMES[f]) == 0) index = i;
    if (running) setMode("static", -1);
    else setMode(FX_NAMES[f], index);
    clearSelection();
    deselectAt = 0;
  }
}

// Farbe der Decke fürs Launchpad: jede Helligkeitsstufe sieht dort sichtbar anders aus
RGB onPad(RGB c, float level) {
  uint8_t m = c.top();
  if (!m) return BLACK;
  float light = powf(m / 255.0f * (0.4f + 0.6f * level), 1.4f);
  int top = max(1, (int)roundf(63 * light));
  auto ch = [&](uint8_t v) { return (uint8_t)ceilf(roundf((float)top * v / m) * 255 / 63.0f); };
  return {ch(c.r), ch(c.g), ch(c.b)};
}

void setupLayout() {
  memset(boxRow, -1, sizeof(boxRow));
  memset(colorAt, -1, sizeof(colorAt));
  memset(funcAt, F_NONE, sizeof(funcAt));
  for (int r = 0; r < ROWS; r++) {
    int y = 9 - r;  // Reihe a = obere Tasten
    for (int c = 0; c < COLS; c++) {
      int i = ledIndex(c + 1, y);
      boxRow[i] = r;
      boxCol[i] = COLS - 1 - c;  // Launchpad ist zur Decke gespiegelt
    }
    colorAt[ledIndex(8, y)] = r;
  }
  funcAt[ledIndex(9, 8)] = F_ON;
  funcAt[ledIndex(9, 7)] = F_OFF;
  funcAt[ledIndex(9, 6)] = F_BRI_UP;
  funcAt[ledIndex(9, 5)] = F_BRI_DOWN;
  for (int k = 0; k < 4; k++) funcAt[ledIndex(9, 4 - k)] = k;
}

void setup() {
  Serial.begin(115200);
  deckLock = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(24, sizeof(Cmd));
  setupLayout();
  startUsb();
  NimBLEDevice::init("Launchpad");
  NimBLEDevice::setMTU(517);
  xTaskCreatePinnedToCore(deckTask, "deck", 8192, nullptr, 1, nullptr, 1);
  startWifi();
  Serial.println("Launchpad-Steuerung gestartet");
}

void loop() {
  static bool layoutSent = false;
  static uint32_t started = millis();
  handleNetwork();
  uint32_t now = millis();
  if (lpGone) {  // Launchpad abgesteckt: sauber neu starten
    Serial.println("Launchpad abgesteckt, starte neu");
    delay(200);
    ESP.restart();
  }

  if (lpReady && !layoutSent) {
    const uint8_t session[] = {0x22, 0x00};   // Session-Layout
    lpCommand(session, 2);
    layoutSent = true;
  }

  uint8_t i;
  while (xQueueReceive(padQueue, &i, 0) == pdTRUE) {
    if (!synced()) continue;
    if (boxRow[i] >= 0) {
      if (deselectAt) { clearSelection(); deselectAt = 0; }  // nach Helligkeit: neue Auswahl
      selected[boxRow[i]][boxCol[i]] ^= true;
    } else if (colorAt[i] >= 0) {
      onColor(COLORS[colorAt[i]]);
      deselectAt = 0;
    } else if (funcAt[i] != F_NONE) {
      onFunction(funcAt[i]);
    }
  }

  if (deselectAt && (int32_t)(now - deselectAt) >= 0) { clearSelection(); deselectAt = 0; }
  // Decke war da und ist seit 45 s weg: neu starten (frische Bluetooth-Verbindung)
  if (everOnline && !otaRunning && now - max(lastOk, started) > 45000) ESP.restart();

  static RGB leds[128];
  static bool use[128];
  memset(use, 0, sizeof(use));
  for (int k = 0; k < 128; k++) {
    if (boxRow[k] >= 0 || colorAt[k] >= 0 || funcAt[k] != F_NONE) use[k] = true;
  }
  if (!synced()) {  // warten, bis die Decke ihren Zustand geschickt hat: Kästen blinken blau
    clearSelection();
    deselectAt = 0;
    RGB wait = (now / 1000) % 2 == 0 ? RGB{0, 0, 40} : BLACK;
    for (int k = 0; k < 128; k++) leds[k] = boxRow[k] >= 0 ? wait : BLACK;
  } else {
    Deck d = snapshot();
    bool blinkOn = (now / 333) % 2 == 0;
    float level = (float)d.brightness / d.maxBrightness;
    const char *mode = d.mode >= 0 && d.mode < NUM_ESP_EFFECTS ? ESP_EFFECTS[d.mode] : "";
    for (int k = 0; k < 128; k++) {
      if (boxRow[k] >= 0) {
        int r = boxRow[k], c = boxCol[k];
        RGB col = onPad(d.frame[r][c], level);
        if (selected[r][c]) col = blinkOn ? (col.top() ? col : RGB{60, 60, 60}) : BLACK;
        leds[k] = col;
      } else if (colorAt[k] >= 0) {
        leds[k] = COLORS[colorAt[k]];
      } else if (funcAt[k] == F_ON) leds[k] = {0, 255, 0};
      else if (funcAt[k] == F_OFF) leds[k] = {255, 0, 0};
      else if (funcAt[k] == F_BRI_UP) leds[k] = WHITE;
      else if (funcAt[k] == F_BRI_DOWN) leds[k] = {40, 40, 40};
      else if (funcAt[k] >= 0 && funcAt[k] < 4) {
        int f = funcAt[k];
        if (strcmp(mode, FX_NAMES[f]) == 0) leds[k] = blinkOn ? FX_COLORS[f] : BLACK;  // laufender Effekt blinkt
        else leds[k] = scaleColor(FX_COLORS[f], 0.35f);
      }
    }
  }
  if (lpReady && layoutSent) showLeds(leds, use);
  delay(10);
}
