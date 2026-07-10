#!/usr/bin/env bash
# bench-check.sh - guided hardware validation for the DJM-T1 support.
#
# Walks the checks that need a hand on the mixer: the unified daemon's live MIDI,
# the 10-bit HID field map, LED feedback, and the DVS input channels. Each step
# prompts you, runs the right tool, and reports what it saw. Safe to re-run; it
# restores the systemd services at the end.
#
# Usage:  tools/bench-check.sh          (run from the repo root or anywhere)
# SPDX-License-Identifier: MIT
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
MIDI="$REPO/midi"
AUDIO="$REPO/audio"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

have() { command -v "$1" >/dev/null 2>&1; }
pause() { printf '\n>>> %s\n    (press Enter when ready) ' "$1"; read -r _; }
hr() { printf '\n========================================================\n'; }

stop_services()  { systemctl --user stop djm-midi.service djmt1-audio.service djm-full.service 2>/dev/null; sleep 1; }
start_services() { systemctl --user start djmt1-audio.service djm-midi.service 2>/dev/null; }

echo "DJM-T1 bench check. This will briefly stop the audio/MIDI services while it"
echo "runs each test, then restart them. Make sure the mixer is plugged in."
pause "Ready to start?"

# Build anything missing.
make -C "$MIDI" >/dev/null 2>&1
make -C "$MIDI" djm_full >/dev/null 2>&1 || echo "(djm_full needs libpipewire-0.3 dev to build)"
make -C "$AUDIO" >/dev/null 2>&1

# ---------------------------------------------------------------- 1. unified daemon
hr; echo "1/4  Unified daemon (djm_full): live MIDI + audio in one process"
if [ -x "$MIDI/djm_full" ]; then
	stop_services
	"$MIDI/djm_full" -v >"$TMP/full.log" 2>&1 &
	FP=$!
	sleep 3
	pause "Move a fader / turn a knob on the mixer for a few seconds"
	sleep 1
	kill "$FP" 2>/dev/null; wait "$FP" 2>/dev/null
	N=$(grep -c '^MIDI:' "$TMP/full.log")
	NODES=$(pw-cli ls Node 2>/dev/null | grep -c 'DJM-T1' || echo 0)
	echo "   -> $N MIDI messages seen, $NODES PipeWire audio nodes were up"
	[ "$N" -gt 0 ] && echo "   PASS: djm_full streams MIDI" || echo "   FAIL: no MIDI (was a control moved? is the mixer armed?)"
else
	echo "   SKIP: djm_full not built"
fi

# ---------------------------------------------------------------- 2. HID 10-bit map
hr; echo "2/4  HID 10-bit field map (which report/offset is which control)"
if [ -x "$MIDI/djm_midi" ] && [ -x "$AUDIO/djmt1-pipewire" ]; then
	stop_services
	"$AUDIO/djmt1-pipewire" >/dev/null 2>&1 &            # audio session (gate)
	AP=$!; sleep 2
	"$MIDI/djm_midi" --hid-probe --no-audio -v >"$TMP/hid.log" 2>&1 &
	MP=$!; sleep 2
	echo "   Move ONE control at a time (fader, EQ, filter, crossfader...);"
	pause "each move prints a 'HID: ..' line and a 'MIDI: ..' line. Do a few, then continue"
	sleep 1
	kill "$MP" "$AP" 2>/dev/null; wait "$MP" "$AP" 2>/dev/null
	echo "   Captured (report bytes alongside MIDI CCs):"
	grep -E '^HID:|^MIDI:' "$TMP/hid.log" | tail -20 | sed 's/^/     /'
	cp "$TMP/hid.log" "$REPO/tools/hid-capture.txt" 2>/dev/null && echo "   saved full capture to tools/hid-capture.txt"
else
	echo "   SKIP: need djm_midi + djmt1-pipewire"
fi

# ---------------------------------------------------------------- 3. LED feedback
hr; echo "3/4  LED feedback (does note 0x2F / 0x30 light the CUE LEDs?)"
if [ -x "$MIDI/djm_midi" ] && have aplaymidi; then
	stop_services
	"$MIDI/djm_midi" >/dev/null 2>&1 &      # full recipe so it's armed + gated open
	MP=$!; sleep 3
	printf 'MThd\x00\x00\x00\x06\x00\x00\x00\x01\x00\x60MTrk\x00\x00\x00\x0c\x00\x90\x2f\x7f\x60\x80\x2f\x00\x00\xff\x2f\x00' > "$TMP/n1.mid"
	printf 'MThd\x00\x00\x00\x06\x00\x00\x00\x01\x00\x60MTrk\x00\x00\x00\x0c\x00\x90\x30\x7f\x60\x80\x30\x00\x00\xff\x2f\x00' > "$TMP/n2.mid"
	echo "   Sending Note 0x2F (CH1 cue) then 0x30 (CH2 cue)..."
	aplaymidi -p "Pioneer DJM-T1" "$TMP/n1.mid" 2>/dev/null; sleep 1
	aplaymidi -p "Pioneer DJM-T1" "$TMP/n2.mid" 2>/dev/null; sleep 1
	pause "Did the CH1 then CH2 CUE LEDs light? (note which, adjust the mapping if not)"
	kill "$MP" 2>/dev/null; wait "$MP" 2>/dev/null
else
	echo "   SKIP: need djm_midi + aplaymidi"
fi

# ---------------------------------------------------------------- 4. DVS channels
hr; echo "4/4  DVS input channels (which USB input pair is which deck's phono)"
if [ -x "$AUDIO/djmt1-capture" ]; then
	stop_services
	echo "   Play a record / signal on ONE deck now."
	pause "Ready? will sample input levels for 4s"
	"$AUDIO/djmt1-capture" 4 -o "$TMP/cap.s24" 2>&1 | grep -A8 'per-channel' | sed 's/^/     /'
	echo "   The channel pair with signal is that deck's input (see docs/dvs-timecode.md)."
else
	echo "   SKIP: djmt1-capture not built"
fi

hr; echo "Restarting services..."
start_services
sleep 3
echo "  djmt1-audio: $(systemctl --user is-active djmt1-audio.service)   djm-midi: $(systemctl --user is-active djm-midi.service)"
echo "Done. Fold any findings into mixxx/Pioneer-DJM-T1.midi.xml, docs/midi-map.md,"
echo "docs/hid-analysis.md, and docs/dvs-timecode.md."
