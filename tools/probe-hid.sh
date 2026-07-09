#!/usr/bin/env bash
# Dump the DJM-T1's HID interface (interface 3) while you move controls, to see
# whether it carries anything the MIDI interface doesn't (extra controls, higher
# resolution, etc.). Reports raw HID reports as hex.
#
# Usage:  sudo tools/probe-hid.sh [seconds]     (default 30)
set -euo pipefail
SECS="${1:-30}"

NODE=""
for h in /sys/class/hidraw/hidraw*; do
  if grep -qi "08E4" "$h/device/uevent" 2>/dev/null; then NODE="/dev/$(basename "$h")"; break; fi
done
[ -n "$NODE" ] || { echo "DJM-T1 HID node not found (plugged in?)"; exit 1; }

OUT="${DJM_HID_CAPTURE:-/tmp/djm-hid.txt}"
echo "Reading $NODE for ${SECS}s -> $OUT"
echo ">>> Move controls / press buttons now. Each HID report prints as hex. <<<"
timeout "$SECS" cat "$NODE" | xxd | tee "$OUT" || true
echo
echo "Done. Distinct report bytes that change with a control => that control is on HID."
