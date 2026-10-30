#!/usr/bin/env python3
"""LED-Decke – Novation Launchpad als Wand-Schaltfeld (läuft auf dem Raspberry Pi).

Belegung (Launchpad liegt normal, Logo oben rechts):
  - linke 7 Spalten, 8 Pad-Reihen + obere Tastenreihe = die 7x9 Kästen
  - 8. Spalte (inkl. oberer Taste) = ganze Querreihe ein/aus
  - rechte Spalte von oben: Helligkeit +, Helligkeit -, Farbverlauf +,
    Farbverlauf -, Rot, Grün, Blau, Weiß

Kasten drücken: er blinkt (ausgewählt), nochmal drücken: abgewählt. Die Tasten
rechts ändern dann Farbe, Farbverlauf und Helligkeit der ausgewählten Kästen.
Ist nichts ausgewählt, ändert Helligkeit +/- die ganze Decke.
Der Pi schickt die Befehle per WLAN (UDP) an den Chef-ESP32 und zeigt auf dem
Launchpad immer die echten Farben der Decke an.
"""
import colorsys
import json
import socket
import sys
import time

import mido

CHEF_HOST = "led-decke-reihe1.local"   # Name des Chef-ESP32 im WLAN
UDP_PORT = 4210
ROWS, COLS = 9, 7
BRIGHTNESS_STEP = 16
HUE_STEP = 1 / 24                      # 24 Schritte = einmal durch den Farbkreis
POLL = 0.1                             # so oft (Sekunden) die Farben der Decke holen

FUNCTIONS = ["bri+", "bri-", "hue+", "hue-", "red", "green", "blue", "white"]
BASIC = {"red": (255, 0, 0), "green": (0, 255, 0), "blue": (0, 0, 255), "white": (255, 255, 255)}


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
        self.sock.settimeout(0.3)
        self.addr = None
        self.state = None
        self.next_lookup = 0

    def _find(self):
        # Den Namen nur alle 30 s nachschlagen (das kann ein paar Sekunden dauern),
        # sonst einfach an alle im WLAN schicken – der ESP32 antwortet trotzdem.
        if time.monotonic() >= self.next_lookup:
            self.next_lookup = time.monotonic() + 30
            try:
                return (socket.gethostbyname(CHEF_HOST), UDP_PORT)
            except OSError:
                pass
        return ("255.255.255.255", UDP_PORT)

    def send(self, cmd):
        """Befehl schicken, Antwort = aktueller Zustand. False, wenn keine Antwort."""
        if self.addr is None:
            self.addr = self._find()
        try:
            self.sock.sendto(json.dumps(cmd).encode(), self.addr)
            data, sender = self.sock.recvfrom(1024)
        except OSError:
            self.addr = None
            return False
        if len(data) != 10 + ROWS * COLS * 6 or data[0] != 1:
            return False
        self.addr = (sender[0], UDP_PORT)
        self.state = {
            "brightness": data[2],
            "max": data[3],
            "boxes": [[tuple(data[10 + (r * COLS + c) * 3:13 + (r * COLS + c) * 3]) for c in range(COLS)]
                      for r in range(ROWS)],
            "frame": [[tuple(data[10 + ROWS * COLS * 3 + (r * COLS + c) * 3:
                                  13 + ROWS * COLS * 3 + (r * COLS + c) * 3]) for c in range(COLS)]
                      for r in range(ROWS)],
        }
        return True


def hexcolor(c):
    return "#%02x%02x%02x" % c


def dim(c, f):
    return tuple(int(v * f) for v in c)


# ---------------------------------------------------------------------
#  Hauptprogramm
# ---------------------------------------------------------------------
def split(c):
    """Farbe -> (Grundfarbe in voller Helligkeit, Helligkeit 0..1)."""
    m = max(c)
    if m == 0:
        return None, 0.0
    return tuple(v * 255 // m for v in c), round(m / 255 * 10) / 10


def scale(base, level):
    return tuple(int(v * level) for v in base)


def main():
    while True:
        try:
            lp = Launchpad()
            break
        except (RuntimeError, OSError, StopIteration) as e:
            print(e, "– neuer Versuch in 5 s", flush=True)
            time.sleep(5)

    chef = Chef()
    selected = set()          # ausgewählte Kästen (blinken)
    remembered = {}           # Grundfarbe von Kästen, die per Helligkeit ganz aus sind
    last_color = BASIC["white"]

    # Tasten-Index -> was sie tun
    box_at, line_at, func_at = {}, {}, {}
    for r in range(ROWS):              # Kasten-Zeile 0 = oben = obere Tastenreihe
        y = 9 - r
        for c in range(COLS):
            box_at[lp.led_index(c + 1, y)] = (r, c)
        line_at[lp.led_index(8, y)] = r
    for k, f in enumerate(FUNCTIONS):  # rechte Spalte, von oben nach unten
        func_at[(8 - k) * 10 + 9] = f

    def apply(changes):
        """changes: {(r, c): farbe} -> an die Decke, gleiche Farben in einem Befehl."""
        by_color = {}
        for cell, col in changes.items():
            by_color.setdefault(col, []).append(list(cell))
        for col, cells in by_color.items():
            chef.send({"op": "boxes", "color": hexcolor(col), "cells": cells})

    def on_function(f, st):
        nonlocal last_color
        if not selected:
            if f in ("bri+", "bri-") and st:   # nichts ausgewählt: ganze Decke heller/dunkler
                step = BRIGHTNESS_STEP if f == "bri+" else -BRIGHTNESS_STEP
                chef.send({"op": "settings", "brightness": max(0, min(st["max"], st["brightness"] + step))})
            elif f in BASIC:
                last_color = BASIC[f]
            return
        if not st:
            return
        changes = {}
        for r, c in selected:
            base, level = split(st["boxes"][r][c])
            if f in BASIC:
                last_color = BASIC[f]
                changes[(r, c)] = scale(BASIC[f], level if level > 0 else 1.0)
            elif f in ("hue+", "hue-") and base:
                h, sat, _ = colorsys.rgb_to_hsv(*[v / 255 for v in base])
                if sat < 0.05:                 # Weiß hat keinen Farbton: bei Rot anfangen
                    h, sat = 0.0, 1.0
                h = (h + (HUE_STEP if f == "hue+" else -HUE_STEP)) % 1
                new_base = tuple(int(v * 255) for v in colorsys.hsv_to_rgb(h, sat, 1))
                changes[(r, c)] = scale(new_base, level)
            elif f in ("bri+", "bri-"):
                if base is None:
                    base = remembered.get((r, c), last_color)
                level = round(min(1.0, max(0.0, level + (0.1 if f == "bri+" else -0.1))), 1)
                if level == 0:
                    remembered[(r, c)] = base
                changes[(r, c)] = scale(base, level)
        apply(changes)

    last_poll = 0
    last_warn = -100
    last_check = 0
    while True:
        try:
            for i in lp.presses():
                if i in box_at:
                    selected ^= {box_at[i]}
                elif i in line_at:
                    row = {(line_at[i], c) for c in range(COLS)}
                    selected = selected - row if row <= selected else selected | row
                elif i in func_at:
                    on_function(func_at[i], chef.state)

            now = time.monotonic()
            if now - last_check >= 2:
                last_check = now
                if not lp.still_there():   # neu eingesteckt -> Dienst startet neu und verbindet sich
                    print("Launchpad abgesteckt, starte neu", flush=True)
                    sys.exit(1)
            if now - last_poll >= POLL:
                last_poll = now
                if not chef.send({"op": "get"}) and chef.state is None and now - last_warn > 30:
                    last_warn = now
                    print("Decke antwortet nicht (WLAN an? ESP32 an?)", flush=True)

            # Launchpad anzeigen: ausgewählte Kästen blinken
            blink_on = int(now * 3) % 2 == 0
            leds = {}
            st = chef.state
            for i, (r, c) in box_at.items():
                col = st["frame"][r][c] if st else (0, 0, 0)
                if (r, c) in selected:
                    col = (col if max(col) > 30 else (90, 90, 90)) if blink_on else (0, 0, 0)
                leds[i] = col
            for i, r in line_at.items():
                full = all((r, c) in selected for c in range(COLS))
                leds[i] = (255, 255, 255) if full and blink_on else (35, 35, 35)
            first = next(iter(selected), None)
            sel_color = split(st["boxes"][first[0]][first[1]])[0] if st and first else None
            for i, f in func_at.items():
                if f in BASIC:
                    leds[i] = BASIC[f]
                elif f.startswith("hue"):
                    leds[i] = sel_color or (60, 60, 60)
                else:
                    leds[i] = (255, 255, 255) if f == "bri+" else (40, 40, 40)
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
