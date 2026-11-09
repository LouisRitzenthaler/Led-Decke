#!/bin/bash
# LED-Decke Mini-System auf dem Raspberry Pi 3 einrichten.
# Aufruf (auf dem Pi, per SSH):
#   curl -sSL https://raw.githubusercontent.com/LouisRitzenthaler/Led-Decke/main/launchpad/mini/install.sh | sudo bash
set -e
REPO=https://raw.githubusercontent.com/LouisRitzenthaler/Led-Decke/main/launchpad/mini

if [ "$(id -u)" != 0 ]; then
    echo "Bitte mit sudo starten"; exit 1
fi

BOOT=/boot
[ -f /boot/firmware/config.txt ] && BOOT=/boot/firmware
CFG=$BOOT/config.txt

case "$(uname -m)" in
    aarch64) BIN=led-mini-aarch64 ;;
    *) BIN=led-mini-armhf ;;
esac

echo "1/5 Programm laden ($BIN)"
mkdir -p /usr/local/lib/led-mini
if [ -n "$LED_MINI_SRC" ]; then   # ohne Internet: Dateien liegen schon auf der SD-Karte
    cp "$LED_MINI_SRC/$BIN" /usr/local/lib/led-mini/init.neu
    cp "$LED_MINI_SRC/led-mini-build" /usr/local/sbin/led-mini-build
else
    curl -sSfL "$REPO/$BIN" -o /usr/local/lib/led-mini/init.neu
    curl -sSfL "$REPO/led-mini-build" -o /usr/local/sbin/led-mini-build
fi
chmod 755 /usr/local/lib/led-mini/init.neu /usr/local/sbin/led-mini-build
mv /usr/local/lib/led-mini/init.neu /usr/local/lib/led-mini/init

echo "2/5 Mini-System bauen"
/usr/local/sbin/led-mini-build

echo "3/5 Nach Kernel-Updates automatisch neu bauen"
mkdir -p /etc/kernel/postinst.d
cat > /etc/kernel/postinst.d/zz-led-mini <<'EOF'
#!/bin/sh
# Nur für die Kernel-Sorte bauen, die dieser Pi gerade benutzt
[ "$(echo "$1" | sed 's/^[0-9.]*//')" = "$(uname -r | sed 's/^[0-9.]*//')" ] || exit 0
/usr/local/sbin/led-mini-build "$1" || true
EOF
chmod 755 /etc/kernel/postinst.d/zz-led-mini

echo "4/5 config.txt anpassen"
[ -f "$CFG.vor-mini" ] || cp "$CFG" "$CFG.vor-mini"
sed -i '/# >>> LED-Decke Mini-System/,/# <<< LED-Decke Mini-System/d' "$CFG"
cat >> "$CFG" <<'EOF'
# >>> LED-Decke Mini-System
[all]
initramfs led-mini.img followkernel
dtparam=krnbt=on
boot_delay=0
disable_splash=1
camera_auto_detect=0
display_auto_detect=0
# <<< LED-Decke Mini-System
EOF
# Bluetooth darf nicht abgeschaltet sein
sed -i -E 's/^([^#]*dtoverlay=(pi3-)?disable-bt.*)$/# \1/' "$CFG"

echo "5/5 Mini-System aktivieren"
rm -f "$BOOT/normal.txt" "$BOOT/NORMAL.TXT" "$BOOT/normal.txt.txt" "$BOOT/normal"
sync

echo
echo "Fertig. Beim nächsten Einschalten startet das Mini-System."
echo "Zurück zum normalen System: am PC die Datei normal.txt auf die SD-Karte (Laufwerk bootfs) legen."
