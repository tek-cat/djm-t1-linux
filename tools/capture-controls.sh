#!/usr/bin/env bash
# Capture the DJM-T1's MIDI so you can identify controls (e.g. the FX section,
# assignable buttons) that aren't in the mapping yet. Arms the mixer, records
# amidi output, then move controls one at a time. Decode with decode-midi.sh.
#
# Usage:  sudo tools/capture-controls.sh [seconds]      (default 90)
set -euo pipefail

DJM_ARM=$(command -v djm_arm || true)
[ -z "$DJM_ARM" ] && DJM_ARM="$(cd "$(dirname "$0")/.." && pwd)/arm/djm_arm"
[ -x "$DJM_ARM" ] || { echo "djm_arm not built. Run: make -C arm"; exit 1; }

CARD=$(for c in /proc/asound/card[0-9]*; do grep -ql DJMT1 "$c/id" 2>/dev/null && basename "$c" | tr -d card; done)
[ -n "${CARD:-}" ] || { echo "DJM-T1 not on host (plugged in?)"; exit 1; }
HW="hw:${CARD},0,0"
SECS="${1:-90}"
OUT="${DJM_CAPTURE:-/tmp/djm-capture.txt}"

echo "Arming..."; "$DJM_ARM" -q
: > "$OUT"
echo
echo "Recording $HW for ${SECS}s -> $OUT"
echo ">>> Move ONE control at a time. Note the order you move them. <<<"
echo ">>> Each control shows a distinct CC or Note number.            <<<"
timeout "$SECS" amidi -p "$HW" -d | tee "$OUT"
echo
echo "Done. Now: tools/decode-midi.sh $OUT"
