#!/usr/bin/env bash
# Discover which MIDI messages light which LEDs on the DJM-T1 (for output/feedback
# mapping). Arms the mixer, then sends each Note-On on channel 1, one at a time,
# so you can watch the panel and record which note number lights which LED.
#
# On most Traktor-era Pioneer gear a button's LED is lit by sending the button's
# own note back, so watch especially around the cue notes (0x2F, 0x30).
#
# Usage:  sudo tools/probe-leds.sh [first] [last]   (default 0 127)
#         Ctrl+C to stop once you've seen enough.
set -euo pipefail

DJM_ARM=$(command -v djm_arm || true)
[ -z "$DJM_ARM" ] && DJM_ARM="$(cd "$(dirname "$0")/.." && pwd)/arm/djm_arm"
[ -x "$DJM_ARM" ] || { echo "djm_arm not built. Run: make -C arm"; exit 1; }

CARD=$(for c in /proc/asound/card[0-9]*; do grep -ql DJMT1 "$c/id" 2>/dev/null && basename "$c" | tr -d card; done)
[ -n "${CARD:-}" ] || { echo "DJM-T1 not on host (plugged in?)"; exit 1; }
HW="hw:${CARD},0,0"
FIRST="${1:-0}"; LAST="${2:-127}"

echo "Arming..."; "$DJM_ARM" -q
echo "Sending Note-On (ch1) for notes $FIRST..$LAST. Watch the panel; note which LED lights."
for n in $(seq "$FIRST" "$LAST"); do
  hx=$(printf '%02X' "$n")
  printf "note 0x%s (%3d) ON  " "$hx" "$n"
  amidi -p "$HW" -S "90 $hx 7F"
  sleep 1.1
  amidi -p "$HW" -S "90 $hx 00"   # off
  printf "off\n"
  sleep 0.25
done
echo "Done. Report which note numbers lit which LEDs and I'll add <output> blocks to the mapping."
