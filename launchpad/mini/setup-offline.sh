#!/bin/bash
# Läuft einmal beim Start des Pi (über cmdline.txt), ohne Internet.
# Richtet das Mini-System aus den Dateien auf der SD-Karte ein und startet neu.
D=/boot/firmware/led-mini
[ -d "$D" ] || D=/boot/led-mini
CMD=/boot/firmware/cmdline.txt
[ -f "$CMD" ] || CMD=/boot/cmdline.txt
exec > "$D/protokoll.txt" 2>&1
# Den Eintrag in cmdline.txt sofort wieder entfernen, damit das nur einmal läuft
sed -i 's| *systemd\.run=[^ ]*||g; s| *systemd\.run_success_action=[^ ]*||g; s| *systemd\.unit=kernel-command-line\.target||g' "$CMD"
echo "cmdline.txt jetzt:"; cat "$CMD"
LED_MINI_SRC="$D" bash "$D/install.sh"
echo "Ergebnis: $?"
sync
exit 0
