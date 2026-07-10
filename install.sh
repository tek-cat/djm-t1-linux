#!/usr/bin/env bash
# Install djm-t1-linux: the MIDI bridge (djm_midi), the audio driver
# (djmt1-pipewire), the udev access rule, the systemd user services, and the
# Mixxx mapping. Prompts for sudo only where needed (binaries + udev rule).
set -euo pipefail
cd "$(dirname "$0")"
PREFIX="${PREFIX:-/usr/local}"

echo "==> Building (needs libusb-1.0, alsa-lib, and libpipewire-0.3 dev headers)"
make -C arm
make -C midi
make -C audio djmt1-pipewire

echo "==> Installing binaries to $PREFIX/bin (sudo)"
sudo make -C arm  install PREFIX="$PREFIX"
sudo make -C midi install PREFIX="$PREFIX"
sudo install -Dm755 audio/djmt1-pipewire "$PREFIX/bin/djmt1-pipewire"

echo "==> Installing udev access rule (sudo)"
# Non-root access to the mixer so the user services can claim it. The arm is now
# done by djm_midi itself, so no auto-arm service is needed.
sudo install -Dm644 udev/99-djm-t1.rules /etc/udev/rules.d/99-djm-t1.rules
sudo udevadm control --reload
sudo udevadm trigger --attr-match=idVendor=08e4 || true

echo "==> Installing systemd USER services (no sudo)"
# audio (PipeWire device) + MIDI bridge. The MIDI service runs with --no-audio so
# it leaves the audio interface for the audio service; the two run side by side.
install -Dm644 audio/systemd/djmt1-audio.service "$HOME/.config/systemd/user/djmt1-audio.service"
install -Dm644 midi/systemd/djm-midi.service     "$HOME/.config/systemd/user/djm-midi.service"
systemctl --user daemon-reload
systemctl --user enable --now djmt1-audio.service djm-midi.service

echo "==> Installing Mixxx mapping to ~/.mixxx/controllers"
mkdir -p "$HOME/.mixxx/controllers"
cp mixxx/Pioneer-DJM-T1.midi.xml mixxx/Pioneer-DJM-T1-scripts.js "$HOME/.mixxx/controllers/"

cat <<'EOF'

Done. The mixer now works as a MIDI controller AND a soundcard on Linux.

  1. Make sure you're in the "audio" group (non-root device access): groups | grep audio
     (if you just got added, log out/in once)
  2. Verify the services are up:  systemctl --user status djm-midi djmt1-audio
  3. Confirm MIDI:  aseqdump -p "Pioneer DJM-T1"   then move a fader
  4. In Mixxx: Preferences -> Controllers, select "Pioneer DJM-T1", Enable it,
     and Load the "Pioneer DJM-T1" mapping. Audio shows up as a PipeWire device.

MIDI-only, no audio device? Run `djm_midi` without --no-audio (it keeps its own
audio session alive internally) and skip the audio service.
EOF
