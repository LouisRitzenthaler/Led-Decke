#!/bin/bash
# Raspberry Pi so einstellen, dass das Launchpad so schnell wie möglich nach dem Einschalten läuft.
# Aufruf:  curl -sSL https://raw.githubusercontent.com/LouisRitzenthaler/Led-Decke/main/launchpad/fastboot.sh | bash
set -eE
trap 'echo; echo "FEHLER in Zeile $LINENO - bitte Foto an Claude schicken"' ERR

main() {
BASE=https://raw.githubusercontent.com/LouisRitzenthaler/Led-Decke/main/launchpad
DIR="$HOME/led-decke-launchpad"
USER_NAME="$(whoami)"

echo "== Neuestes Launchpad-Programm holen =="
mkdir -p "$DIR"
curl -sSL "$BASE/launchpad.py" -o "$DIR/launchpad.py"
python3 -m py_compile "$DIR/launchpad.py"

echo "== Start-Einstellungen (config.txt / cmdline.txt) =="
CFG=/boot/firmware/config.txt; [ -f "$CFG" ] || CFG=/boot/config.txt
CMD=/boot/firmware/cmdline.txt; [ -f "$CMD" ] || CMD=/boot/cmdline.txt
sudo cp -n "$CFG" "$CFG.vor-schnellstart" || true
sudo cp -n "$CMD" "$CMD.vor-schnellstart" || true
for L in "boot_delay=0" "disable_splash=1" "initial_turbo=30" "camera_auto_detect=0" "display_auto_detect=0"; do
  KEY="${L%%=*}"
  if grep -q "^$KEY=" "$CFG"; then sudo sed -i "s/^$KEY=.*/$L/" "$CFG"; else echo "$L" | sudo tee -a "$CFG" > /dev/null; fi
done
# Weniger Text beim Start, kein Startbild
sudo sed -i 's/ splash//; s/ plymouth.ignore-serial-consoles//' "$CMD"
grep -q " quiet" "$CMD" || sudo sed -i '1 s/$/ quiet/' "$CMD"
grep -q "loglevel=" "$CMD" || sudo sed -i '1 s/$/ loglevel=3/' "$CMD"

echo "== Unnötige Dienste abschalten =="
sudo systemctl set-default multi-user.target
for S in NetworkManager-wait-online.service systemd-networkd-wait-online.service \
         apt-daily.timer apt-daily-upgrade.timer apt-daily.service apt-daily-upgrade.service \
         man-db.timer e2scrub_reap.service dphys-swapfile.service keyboard-setup.service \
         triggerhappy.service triggerhappy.socket ModemManager.service cups.service cups-browsed.service \
         rpi-eeprom-update.service raspi-config.service lightdm.service plymouth-start.service \
         plymouth-read-write.service plymouth-quit.service plymouth-quit-wait.service \
         console-setup.service sshswitch.service; do
  sudo systemctl disable "$S" > /dev/null 2>&1 || true
done
sudo systemctl mask NetworkManager-wait-online.service systemd-networkd-wait-online.service > /dev/null 2>&1 || true

echo "== Launchpad-Dienst ganz früh starten =="
sudo tee /etc/systemd/system/led-decke-launchpad.service > /dev/null <<UNIT
[Unit]
Description=LED-Decke Launchpad
Wants=bluetooth.service
After=bluetooth.service hciuart.service

[Service]
User=$USER_NAME
SupplementaryGroups=audio bluetooth
ExecStart=/usr/bin/python3 $DIR/launchpad.py
Restart=always
RestartSec=1
Nice=-5

[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl daemon-reload
sudo systemctl enable led-decke-launchpad
sudo systemctl enable bluetooth hciuart > /dev/null 2>&1 || true

echo
echo "Fertig. Jetzt: sudo reboot"
echo "Nach dem Neustart zum Messen:  systemd-analyze   und   journalctl -u led-decke-launchpad -b | head -20"
}

main "$@" < /dev/null
