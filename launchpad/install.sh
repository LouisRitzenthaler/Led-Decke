#!/bin/bash
# LED-Decke Launchpad auf dem Raspberry Pi einrichten.
# Aufruf:  curl -sSL https://raw.githubusercontent.com/LouisRitzenthaler/Led-Decke/main/launchpad/install.sh | bash
set -e
trap 'echo; echo "FEHLER in Zeile $LINENO - bitte Foto an Claude schicken"' ERR

# Alles in einer Funktion, damit "curl | bash" sicher das ganze Skript liest
main() {
BASE=https://raw.githubusercontent.com/LouisRitzenthaler/Led-Decke/main/launchpad
DIR="$HOME/led-decke-launchpad"
USER_NAME="$(whoami)"

echo "== Pakete installieren =="
sudo apt-get update -q || echo "WARNUNG: apt-get update ging nicht (Internet da?)"
sudo apt-get install -y avahi-daemon libnss-mdns || true
if ! sudo apt-get install -y python3-mido python3-rtmidi; then
  echo "== Pakete nicht gefunden, nehme pip =="
  sudo apt-get install -y python3-pip libasound2-dev libjack-dev
  sudo pip3 install mido python-rtmidi --break-system-packages 2>/dev/null || sudo pip3 install mido python-rtmidi
fi
python3 -c "import mido, rtmidi" || { echo "FEHLER: mido/rtmidi fehlen"; exit 1; }

# Das WLAN, in dem der Pi gerade ist (der Hotspot), beim Start bevorzugen
if command -v nmcli > /dev/null; then
  WLAN="$(nmcli -t -f NAME,TYPE connection show --active | grep ':802-11-wireless$' | head -1 | cut -d: -f1)"
  if [ -n "$WLAN" ]; then
    sudo nmcli connection modify "$WLAN" connection.autoconnect-priority 10 || true
    echo "WLAN \"$WLAN\" wird beim Start bevorzugt"
  fi
fi

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
