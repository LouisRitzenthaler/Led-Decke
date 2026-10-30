#!/usr/bin/env python3
"""LED-Decke – Novation Launchpad als Wand-Schaltfeld (läuft auf dem Raspberry Pi).

Belegung (Spalten 1-9 von links, Reihen a-i von oben, a = obere runde Tasten):
  - 1-7 / a-i   = die 7x9 Kästen. Drücken: blinkt = ausgewählt, nochmal: abgewählt
  - Spalte 8    = 9 Farben (a Rot ... i Weiß) für die ausgewählten Kästen
  - 9b / 9c     = An / Aus (ausgewählte Kästen, ohne Auswahl die ganze Decke)
  - 9d / 9e     = Helligkeit + / - (ausgewählte Kästen, ohne Auswahl die ganze Decke)
  - 9f - 9i     = Effekte. Nochmal drücken = Effekt aus

Nach einer Farbe ist die Auswahl sofort weg, nach Helligkeit 2,5 s nach dem
letzten Drücken (so kann man mehrmals drücken).
Der Pi schickt die Befehle per Bluetooth an den ESP32 (ohne WLAN, ohne Internet)
und zeigt auf dem Launchpad die echten Farben der Decke. Fehlt das Bluetooth-Paket
(bleak), nimmt er das WLAN (UDP). Solange die Decke nicht verbunden ist, blinken
die Kästen auf dem Launchpad langsam blau und Tasten tun nichts. Danach zeigt
es genau das, was die Decke gerade macht.
"""
import json
import math
import queue
import socket
import sys
import threading
import time

import mido

try:
    import asyncio
    from bleak import BleakClient, BleakScanner
except ImportError:
    BleakClient = None

BLE_NAME = "LED-Decke"                 # Bluetooth-Name des ESP32
BLE_SERVICE = "6e1d0001-8a3c-4c5b-9b1e-1ed0c0dec0de"
BLE_CMD = "6e1d0002-8a3c-4c5b-9b1e-1ed0c0dec0de"
BLE_STATE = "6e1d0003-8a3c-4c5b-9b1e-1ed0c0dec0de"
CHEF_HOST = "led-decke-reihe1.local"   # Name des ESP32 im WLAN (nur ohne Bluetooth)
UDP_PORT = 4210
ROWS, COLS = 9, 7
MAX_BRIGHTNESS = 160                   # wie im ESP32
BRIGHTNESS_STEP = 16
POLL = 0.1                             # so oft (Sekunden) die Farben der Decke holen
DESELECT_AFTER = 2.5                   # Auswahl endet so viele Sekunden nach Helligkeit

# Spalte 8, von a bis i
COLORS = [(255, 0, 0), (255, 90, 0), (255, 210, 0), (0, 255, 0), (0, 255, 200),
          (0, 0, 255), (140, 0, 255), (255, 0, 140), (255, 255, 255)]
# Spalte 9, von f bis i: (Name im ESP32, Farbe der Taste)
EFFECTS = [("rainbow", (255, 0, 255)), ("colorfade", (0, 255, 255)), ("wave", (0, 80, 255)),
           ("sparkle", (255, 255, 255))]
# Reihenfolge der Effekte im ESP32 (für die Nummer im Zustand)
ESP_EFFECTS = ["rainbow", "colorfade", "wave", "breathe", "sparkle", "rain", "wiring_test"]


# ---------------------------------------------------------------------
#  Launchpad
# ---------------------------------------------------------------------
class Launchpad:
    """Unterstützt Launchpad Mini MK3, Launchpad X und Launchpad MK2."""

    def __init__(self):
        names = [n for n in mido.get_output_names() if "launchpad" in n.lower() or "lpx" in n.lower()
                 or "lpminimk3" in n.lower()]
        if not names:
            raise RuntimeError("Kein Launchpad gefunden – ist es per USB am Pi?")
        # Mini MK3 und X haben zwei Anschlüsse (DAW und MIDI): wir nehmen MIDI.
        name = next((n for n in names if "midi" in n.lower() and "daw" not in n.lower()), names[0])
        low = name.lower()
        if "mk2" in low:
            self.kind, self.dev, self.scale, self.top = "mk2", 0x18, 63, 104
        elif "lpx" in low or "launchpad x" in low:
            self.kind, self.dev, self.scale, self.top = "x", 0x0C, 127, 91
        else:
            self.kind, self.dev, self.scale, self.top = "mk3", 0x0D, 127, 91
        in_name = next((n for n in mido.get_input_names() if n == name), None) or \
            next(n for n in mido.get_input_names() if name.split(":")[0] in n)
        self.name = name
        self.shown = {}
        self.out = mido.open_output(name)
        self.inp = mido.open_input(in_name)
        if self.kind == "mk2":
            self._sysex([0x22, 0x00])            # Session-Layout
        else:
            self._sysex([0x0E, 0x01])            # Programmer-Modus
        print(f"Launchpad gefunden: {name} ({self.kind})", flush=True)

    def _sysex(self, data):
        self.out.send(mido.Message("sysex", data=[0x00, 0x20, 0x29, 0x02, self.dev] + data))

    def led_index(self, x, y):
        """x = Spalte 1..9 von links, y = Reihe 1..9 von unten (9 = obere Tastenreihe)."""
        if y == 9:
            return self.top + x - 1
        return y * 10 + x

    def show(self, leds):
        """leds: {index: (r, g, b)} mit 0..255. Schickt nur, was sich geändert hat."""
        leds = {i: c for i, c in leds.items() if self.shown.get(i) != c}
        if not leds:
            return
        self.shown.update(leds)
        s = self.scale
        items = list(leds.items())
        for k in range(0, len(items), 8):   # in kleinen Paketen, lange SysEx gehen am Pi verloren
            if self.kind == "mk2":
                data = [0x0B]
                for i, (r, g, b) in items[k:k + 8]:
                    data += [i, r * s // 255, g * s // 255, b * s // 255]
            else:
                data = [0x03]
                for i, (r, g, b) in items[k:k + 8]:
                    data += [0x03, i, r * s // 255, g * s // 255, b * s // 255]
            self._sysex(data)

    def still_there(self):
        """False, wenn das Launchpad abgesteckt (oder neu eingesteckt) wurde."""
        return self.name in mido.get_output_names()

    def presses(self):
        """Gedrückte Tasten als Index (wie led_index)."""
        for msg in self.inp.iter_pending():
            if msg.type == "note_on" and msg.velocity > 0:
                yield msg.note
            elif msg.type == "control_change" and msg.value > 0:
                yield msg.control


# ---------------------------------------------------------------------
#  Verbindung zum Chef-ESP32
# ---------------------------------------------------------------------
class Chef:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.settimeout(0.8)     # WLAN mit Bluetooth am ESP32 antwortet manchmal langsam
        self.addr = None          # zuletzt bekannte Adresse des ESP32
        self.named = None         # Adresse über den Namen (wird nebenbei gesucht)
        self.misses = 0
        self.state = None
        self.online = False
        self.ever_online = False
        self.last_ok = 0
        self.todo = queue.Queue()
        threading.Thread(target=self._run, daemon=True).start()
        threading.Thread(target=self._lookup, daemon=True).start()

    def post(self, cmd):
        """Befehl im Hintergrund schicken – das Launchpad wartet nie auf die Decke."""
        self.todo.put(cmd)

    def _run(self):
        while True:
            try:
                cmd = self.todo.get(timeout=POLL)
            except queue.Empty:
                cmd = {"op": "get"}
            ok = self.send(cmd)
            if not ok and cmd["op"] != "get":
                ok = self.send(cmd)            # verlorenen Befehl einmal wiederholen
            self.online = ok
            self.ever_online = self.ever_online or ok

    def _lookup(self):
        # Den Namen nebenbei nachschlagen (kann Sekunden dauern), damit nichts hängt
        while True:
            try:
                self.named = (socket.gethostbyname(CHEF_HOST), UDP_PORT)
            except OSError:
                pass
            time.sleep(30)

    def send(self, cmd):
        """Befehl schicken, Antwort = aktueller Zustand. False, wenn keine Antwort."""
        addr = self.addr or self.named or ("255.255.255.255", UDP_PORT)
        self.sock.setblocking(False)          # alte, zu spät gekommene Antworten wegwerfen
        try:
            while self.sock.recv(1024):
                pass
        except OSError:
            pass
        self.sock.settimeout(0.8)
        try:
            self.sock.sendto(json.dumps(cmd, separators=(",", ":")).encode(), addr)
            while True:
                data, sender = self.sock.recvfrom(1024)
                if len(data) == 10 + ROWS * COLS * 6 and data[0] == 1:
                    break
        except OSError:
            self.misses += 1
            if self.misses >= 3:               # ESP32 hat neue Adresse? Neu suchen
                self.addr = None
            return False
        self.misses = 0
        self.addr = (sender[0], UDP_PORT)
        self.state = parse_state(data)
        self.last_ok = time.monotonic()
        return True

    def synced(self):
        """True, wenn die Decke in den letzten 3 s ihren Zustand geschickt hat."""
        return self.state is not None and time.monotonic() - self.last_ok < 3


class BleChef:
    """Verbindung zum ESP32 per Bluetooth – gleiche Bedienung wie Chef."""

    def __init__(self):
        self.state = None
        self.online = False
        self.ever_online = False
        self.last_ok = 0
        self.todo = queue.Queue()
        self.pending = None
        threading.Thread(target=lambda: asyncio.run(self._run()), daemon=True).start()

    def post(self, cmd):
        """Befehl im Hintergrund schicken – das Launchpad wartet nie auf die Decke."""
        self.todo.put(cmd)

    async def _run(self):
        while True:                    # suchen, verbinden, bei Abbruch von vorn
            try:
                dev = await BleakScanner.find_device_by_filter(
                    lambda d, ad: d.name == BLE_NAME or BLE_SERVICE in (ad.service_uuids or []),
                    timeout=10)
                if dev is None:
                    continue
                async with BleakClient(dev) as client:
                    print("Decke per Bluetooth verbunden", flush=True)
                    while not self.todo.empty():   # alte Tasten von vor der Verbindung vergessen
                        self.todo.get_nowait()
                    self.pending = None
                    await self._talk(client)
            except Exception as e:     # Bluetooth-Fehler: kurz warten, neu verbinden
                print("Bluetooth:", e, flush=True)
            self.online = False
            self.last_ok = 0
            await asyncio.sleep(1)

    def synced(self):
        """True, wenn die Decke in den letzten 3 s ihren Zustand geschickt hat."""
        return self.state is not None and time.monotonic() - self.last_ok < 3

    async def _talk(self, client):
        await self._read(client)             # zuerst den Zustand der Decke holen
        last_read = time.monotonic()
        while client.is_connected:
            if self.pending is None:
                try:
                    self.pending = self.todo.get_nowait()
                except queue.Empty:
                    pass
            if self.pending is not None:    # bleibt gemerkt, bis er sicher angekommen ist
                await client.write_gatt_char(BLE_CMD, json.dumps(self.pending, separators=(",", ":")).encode(),
                                             response=True)
                self.pending = None
                continue
            if time.monotonic() - last_read >= POLL:
                last_read = time.monotonic()
                await self._read(client)
            else:
                await asyncio.sleep(0.01)

    async def _read(self, client):
        data = bytes(await client.read_gatt_char(BLE_STATE))
        st = parse_state(data)
        if st is None:
            print("Zustand der Decke unlesbar (%d Bytes)" % len(data), flush=True)
            return
        self.state = st
        self.last_ok = time.monotonic()
        self.online = self.ever_online = True


def parse_state(data):
    """Zustand vom ESP32 (Bytes) -> dict, oder None wenn kaputt."""
    if len(data) != 10 + ROWS * COLS * 6 or data[0] != 1:
        return None
    return {
        "mode": data[1] - 256 if data[1] > 127 else data[1],
        "brightness": data[2],
        "max": data[3],
        "boxes": [[tuple(data[10 + (r * COLS + c) * 3:13 + (r * COLS + c) * 3]) for c in range(COLS)]
                  for r in range(ROWS)],
        "frame": [[tuple(data[10 + ROWS * COLS * 3 + (r * COLS + c) * 3:
                              13 + ROWS * COLS * 3 + (r * COLS + c) * 3]) for c in range(COLS)]
                  for r in range(ROWS)],
    }


def hexcolor(c):
    return "#%02x%02x%02x" % c


def dim(c, f):
    return tuple(int(v * f) for v in c)


# ---------------------------------------------------------------------
#  Farben umrechnen
# ---------------------------------------------------------------------
def split(c):
    """Farbe -> (Grundfarbe in voller Helligkeit, Helligkeit 0..1)."""
    m = max(c)
    if m == 0:
        return None, 0.0
    return tuple(v * 255 // m for v in c), round(m / 255 * 10) / 10


def scale(base, level):
    return tuple(int(v * level) for v in base)


def on_pad(c, level):
    """Farbe der Decke fürs Launchpad: jede Helligkeitsstufe sieht dort sichtbar anders aus."""
    m = max(c)
    if m == 0:
        return (0, 0, 0)
    light = (m / 255 * (0.4 + 0.6 * level)) ** 1.4      # 0..1, wie das Auge Helligkeit sieht
    top = max(1, round(63 * light))                      # Launchpad kann 0..63
    return tuple(math.ceil(round(top * v / m) * 255 / 63) for v in c)


# ---------------------------------------------------------------------
#  Hauptprogramm
# ---------------------------------------------------------------------
def main():
    while True:
        try:
            lp = Launchpad()
            break
        except (RuntimeError, OSError, StopIteration) as e:
            print(e, "– neuer Versuch in 5 s", flush=True)
            time.sleep(5)

    chef = BleChef() if BleakClient else Chef()
    selected = set()          # ausgewählte Kästen (blinken)
    remembered = {}           # Grundfarbe von Kästen, die per Helligkeit ganz aus sind
    deselect_at = None        # Zeitpunkt, an dem die Auswahl nach Helligkeit endet

    def deck():
        return chef

    # Tasten-Index -> was sie tun. Reihe a = obere Tasten (y=9), i = unten (y=1)
    box_at, color_at, func_at = {}, {}, {}
    for r in range(ROWS):
        y = 9 - r
        for c in range(COLS):
            box_at[lp.led_index(c + 1, y)] = (r, COLS - 1 - c)   # Launchpad ist zur Decke gespiegelt (1-7 andersherum)
        color_at[lp.led_index(8, y)] = COLORS[r]
    func_at[lp.led_index(9, 8)] = "on"
    func_at[lp.led_index(9, 7)] = "off"
    func_at[lp.led_index(9, 6)] = "bri+"
    func_at[lp.led_index(9, 5)] = "bri-"
    for k, (name, _) in enumerate(EFFECTS):
        func_at[lp.led_index(9, 4 - k)] = name
    effect_color = dict(EFFECTS)

    def apply(d, changes):
        """changes: {(r, c): farbe} -> an die Decke, gleiche Farben in einem Befehl."""
        by_color = {}
        for cell, col in changes.items():
            by_color.setdefault(col, []).append(list(cell))
        for col, cells in by_color.items():
            if len(cells) == ROWS * COLS:
                d.post({"op": "fill", "color": hexcolor(col)})
                continue
            for k in range(0, len(cells), 20):     # der ESP32 nimmt max. 512 Zeichen pro Befehl
                d.post({"op": "boxes", "color": hexcolor(col), "cells": cells[k:k + 20]})
        st = d.state
        if st and d is chef:           # sofort merken, damit schnelles Drücken nicht verloren geht
            for (r, c), col in changes.items():
                st["boxes"][r][c] = col
            st["mode"] = -1

    def on_color(col):
        d = deck()
        st = d.state
        if not selected:               # nichts ausgewählt: Farbe für Effekte wie Welle/Atmen
            d.post({"op": "settings", "color": hexcolor(col)})
            return
        if st:
            apply(d, {(r, c): scale(col, split(st["boxes"][r][c])[1] or 1.0) for r, c in selected})
        selected.clear()

    saved = {}                # Kästen vor "Alles aus", für "Alles an"

    def on_off(d, st, on):
        nonlocal saved
        boxes = st["boxes"]
        if selected:
            changes = {}
            for r, c in selected:
                base, level = split(boxes[r][c])
                if on and base is None:
                    changes[(r, c)] = remembered.pop((r, c), (255, 255, 255))
                elif not on and base is not None:
                    remembered[(r, c)] = boxes[r][c]
                    changes[(r, c)] = (0, 0, 0)
            apply(d, changes)
        elif not on:                   # ganze Decke aus, vorher merken
            lit = {(r, c): boxes[r][c] for r in range(ROWS) for c in range(COLS) if max(boxes[r][c]) > 0}
            if lit:
                saved = lit
            apply(d, {(r, c): (0, 0, 0) for r in range(ROWS) for c in range(COLS)})
        else:                          # ganze Decke wieder an (wie vorher, sonst weiß), Effekt aus
            if any(max(boxes[r][c]) > 0 for r in range(ROWS) for c in range(COLS)):
                if st["mode"] >= 0:
                    d.post({"op": "settings", "mode": "static"})
                    if d is chef:
                        st["mode"] = -1
                return
            apply(d, saved or {(r, c): (255, 255, 255) for r in range(ROWS) for c in range(COLS)})

    def on_function(f):
        nonlocal deselect_at
        d = deck()
        st = d.state
        if not st:
            return
        if f in ("on", "off"):
            on_off(d, st, f == "on")
            selected.clear()
            deselect_at = None
        elif f in ("bri+", "bri-"):
            step = 1 if f == "bri+" else -1
            if not selected:           # ganze Decke heller/dunkler
                bri = max(0, min(st["max"], st["brightness"] + step * BRIGHTNESS_STEP))
                d.post({"op": "settings", "brightness": bri})
                if d is chef:
                    st["brightness"] = bri
                return
            changes = {}
            for r, c in selected:
                base, level = split(st["boxes"][r][c])
                if base is None:
                    base = remembered.get((r, c), (255, 255, 255))
                level = round(min(1.0, max(0.0, level + step * 0.1)), 1)
                if level == 0:
                    remembered[(r, c)] = base
                changes[(r, c)] = scale(base, level)
            apply(d, changes)
            deselect_at = time.monotonic() + DESELECT_AFTER
        else:                          # Effekt: an, oder aus wenn er schon läuft
            running = st["mode"] >= 0 and ESP_EFFECTS[st["mode"]] == f
            d.post({"op": "settings", "mode": "static" if running else f})
            if d is chef:
                st["mode"] = -1 if running else ESP_EFFECTS.index(f)
            selected.clear()
            deselect_at = None

    last_warn = time.monotonic() - 25
    last_check = 0
    while True:
        try:
            for i in lp.presses():
                if not chef.synced():
                    continue
                if i in box_at:
                    if deselect_at:            # nach Helligkeit: neue Auswahl beginnt
                        selected.clear()
                        deselect_at = None
                    selected ^= {box_at[i]}
                elif i in color_at:
                    on_color(color_at[i])
                    deselect_at = None
                elif i in func_at:
                    on_function(func_at[i])

            now = time.monotonic()
            if deselect_at and now >= deselect_at:
                selected.clear()
                deselect_at = None
            if now - last_check >= 2:
                last_check = now
                if not lp.still_there():   # neu eingesteckt -> Dienst startet neu und verbindet sich
                    print("Launchpad abgesteckt, starte neu", flush=True)
                    sys.exit(1)
            if not chef.synced():          # warten, bis die Decke ihren Zustand geschickt hat
                if now - last_warn > 30:
                    last_warn = now
                    print("Warte auf die Decke ...", flush=True)
                selected.clear()
                deselect_at = None
                wait = (0, 0, 40) if int(now) % 2 == 0 else (0, 0, 0)
                lp.show({**{i: wait for i in box_at}, **{i: (0, 0, 0) for i in color_at},
                         **{i: (0, 0, 0) for i in func_at}})
                time.sleep(0.05)
                continue

            # Launchpad anzeigen: Farben der Decke (mit Helligkeit), Auswahl blinkt
            blink_on = int(now * 3) % 2 == 0
            st = deck().state
            leds = {}
            level = st["brightness"] / st["max"] if st else 1.0
            for i, (r, c) in box_at.items():
                col = on_pad(st["frame"][r][c], level) if st else (0, 0, 0)
                if (r, c) in selected:     # blinkt in seiner echten Helligkeit, aus = grau
                    col = (col if max(col) > 0 else (60, 60, 60)) if blink_on else (0, 0, 0)
                leds[i] = col
            for i, col in color_at.items():
                leds[i] = col
            mode = ESP_EFFECTS[st["mode"]] if st and 0 <= st["mode"] < len(ESP_EFFECTS) else None
            for i, f in func_at.items():
                if f == "on":
                    leds[i] = (0, 255, 0)
                elif f == "off":
                    leds[i] = (255, 0, 0)
                elif f == "bri+":
                    leds[i] = (255, 255, 255)
                elif f == "bri-":
                    leds[i] = (40, 40, 40)
                elif f == mode:            # laufender Effekt blinkt
                    leds[i] = effect_color[f] if blink_on else (0, 0, 0)
                else:
                    leds[i] = dim(effect_color[f], 0.35)
            lp.show(leds)
            time.sleep(0.01)
        except OSError as e:           # Launchpad abgesteckt o. Ä. -> Dienst startet neu
            print("Fehler:", e, flush=True)
            time.sleep(2)
            sys.exit(1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
