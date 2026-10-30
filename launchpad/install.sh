#!/bin/bash
# LED-Decke Launchpad auf dem Raspberry Pi einrichten.
# Aufruf:  curl -sSL https://raw.githubusercontent.com/LouisRitzenthaler/Led-Decke/main/launchpad/install.sh | bash
set -e

# Alles in einer Funktion, damit "curl | bash" sicher das ganze Skript liest
main() {
BASE=https://raw.githubusercontent.com/LouisRitzenthaler/Led-Decke/main/launchpad
DIR="$HOME/led-decke-launchpad"
USER_NAME="$(whoami)"

echo "== Pakete installieren =="
sudo apt-get update -q
sudo apt-get install -y python3-mido python3-rtmidi avahi-daemon libnss-mdns

echo "== Programm herunterladen =="
mkdir -p "$DIR"
curl -sSL "$BASE/launchpad.py" -o "$DIR/launchpad.py"

echo "== Autostart einrichten =="
sudo tee /etc/systemd/system/led-decke-launchpad.service > /dev/null <<UNIT
[Unit]
Description=LED-Decke Launchpad
After=network-online.target sound.target
Wants=network-online.target

[Service]
User=$USER_NAME
SupplementaryGroups=audio
ExecStart=/usr/bin/python3 $DIR/launchpad.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl daemon-reload
sudo systemctl enable led-decke-launchpad
sudo systemctl restart led-decke-launchpad
sleep 3
sudo systemctl --no-pager status led-decke-launchpad | tail -5
echo
echo "Fertig. Das Launchpad startet ab jetzt automatisch mit dem Pi."
}

main "$@" < /dev/null
