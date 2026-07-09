#!/usr/bin/env bash
# Install djm-t1-linux: the arm tool, udev auto-arm rule, systemd service, and
# the Mixxx mapping. Prompts for sudo where needed.
set -euo pipefail
cd "$(dirname "$0")"
PREFIX="${PREFIX:-/usr/local}"

echo "==> Building djm_arm"
make -C arm

echo "==> Installing djm_arm to $PREFIX/bin (sudo)"
sudo make -C arm install PREFIX="$PREFIX"

echo "==> Installing udev rule + systemd service (sudo)"
sudo install -Dm644 udev/99-djm-t1.rules /etc/udev/rules.d/99-djm-t1.rules
sudo install -Dm644 systemd/djm-t1-arm.service /etc/systemd/system/djm-t1-arm.service
sudo systemctl daemon-reload
sudo udevadm control --reload
sudo udevadm trigger --attr-match=idVendor=08e4 || true

echo "==> Installing Mixxx mapping to ~/.mixxx/controllers"
mkdir -p "$HOME/.mixxx/controllers"
cp mixxx/Pioneer-DJM-T1.midi.xml mixxx/Pioneer-DJM-T1-scripts.js "$HOME/.mixxx/controllers/"

cat <<'EOF'

Done.
  1. Unplug and replug the DJM-T1 (so the auto-arm rule fires), or run: sudo djm_arm
  2. Make sure you're in the "audio" group (for non-root access): groups | grep audio
  3. Start Mixxx with the mixer connected, open Preferences -> Controllers,
     select "PIONEER DJM-T1 MIDI 1", Enable it, and Load the "Pioneer DJM-T1" mapping.

Verify MIDI at any time:  amidi -l   then   amidi -p hw:X,0,0 -d   (move a control)
EOF
