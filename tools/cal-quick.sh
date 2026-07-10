#!/usr/bin/env bash
# cal-quick.sh - fast, verbose range + center capture for the DJM-T1 faders.
#
# Streams every MIDI event live while you move each control, then prints a
# per-control summary: which CC(s) it uses, the observed range, and the RESTING
# value (where you left it). For a detented control (crossfader), park it at the
# physical center last so the resting value reveals the true halfway point (the
# fix for "center sits slightly left in Mixxx": the detent does not land on 64).
#
# ONE long-lived aseqdump runs for the whole session; each control is sliced out
# of the log by line marks. It never re-subscribes to the bridge mid-run, which
# is what silently broke capture on the 2nd control in the restart-per-control
# version. Reads the live djm_midi port; touches no service.
#
#   Usage:  tools/cal-quick.sh            (auto-detects the bridge port)
#           tools/cal-quick.sh 129:0      (force a port)
#
# SPDX-License-Identifier: MIT
set -u

PORT_ARG="${1:-}"
OUT="${CAL_OUT:-/tmp/claude-1000/-home-user-Projects-djm/7769edbd-df1f-4e38-9ac9-fd192234d0fb/scratchpad/cal-quick.txt}"
TTY=/dev/tty

c_hdr()  { printf '\n\033[1;36m========== %s ==========\033[0m\n' "$1"; }
c_dim()  { printf '\033[2m%s\033[0m\n' "$1"; }
c_ok()   { printf '\033[1;32m%s\033[0m\n' "$1"; }
c_warn() { printf '\033[1;33m%s\033[0m\n' "$1"; }

resolve_port() {
	[ -n "$PORT_ARG" ] && { echo "$PORT_ARG"; return; }
	local n
	n=$(aconnect -i | grep -iE "client [0-9]+: .*DJM-T1.*type=user" | head -1 | sed -E 's/client ([0-9]+):.*/\1/')
	[ -z "$n" ] && n=$(aconnect -i | grep -i "DJM-T1" | grep -oE "client [0-9]+" | head -1 | grep -oE "[0-9]+")
	[ -n "$n" ] && echo "${n}:0"
}

PORT="$(resolve_port)"
if [ -z "$PORT" ]; then
	c_warn "No DJM-T1 sequencer port found. Is djm-midi.service running?"
	echo "   systemctl --user start djm-midi.service"
	exit 1
fi

command -v stdbuf >/dev/null && SB="stdbuf -oL -eL" || SB=""

echo "DJM-T1 quick fader calibration"
c_dim "port:   $PORT"
c_dim "output: $OUT"
: > "$OUT"
echo "port=$PORT" >> "$OUT"

# ---- one long-lived reader for the whole session (no per-control re-subscribe) ----
LOG="$(mktemp)"
TAILER=""
cleanup() { [ -n "$TAILER" ] && kill "$TAILER" 2>/dev/null; kill "$ASEQ" 2>/dev/null; rm -f "$LOG"; }
# shellcheck disable=SC2086
$SB aseqdump -p "$PORT" >> "$LOG" 2>&1 &
ASEQ=$!
trap cleanup EXIT INT TERM
tail -n +1 -f "$LOG" 2>/dev/null &
TAILER=$!

summarize() {
	local file="$1" id="$2" detent="$3"
	local pairs notes
	pairs=$(grep -oE 'controller [0-9]+, value [0-9]+' "$file" | sed -E 's/controller ([0-9]+), value ([0-9]+)/\1 \2/')
	notes=$(grep -oE 'Note (on|off) +[0-9]+, note [0-9]+' "$file" | grep -oE 'note [0-9]+' | sort | uniq -c)

	echo
	c_hdr "$id  RESULT"
	if [ -z "$pairs" ] && [ -z "$notes" ]; then
		c_warn "  NONE captured - this control emitted nothing during the window."
		echo "[$id] NONE" >> "$OUT"
		return
	fi
	if [ -n "$pairs" ]; then
		echo "$pairs" | awk -v id="$id" -v detent="$detent" -v out="$OUT" '
		{ cc=$1; v=$2; cnt[cc]++;
		  if(!(cc in mn)||v<mn[cc]) mn[cc]=v;
		  if(!(cc in mx)||v>mx[cc]) mx[cc]=v;
		  last[cc]=v; }
		END{
		  best=""; bc=-1; for(cc in cnt){ if(cnt[cc]>bc){bc=cnt[cc]; best=cc} }
		  for(cc in cnt){
		    off=last[cc]-64;
		    printf "  CC %-3d (0x%02X): %4d msgs   range %3d..%-3d   resting %3d", cc,cc,cnt[cc],mn[cc],mx[cc],last[cc];
		    if(detent=="1") printf "   center offset %+d from 64", off;
		    if(cc==best) printf "   <- primary";
		    printf "\n";
		    printf "[%s] cc=%d count=%d min=%d max=%d resting=%d\n", id,cc,cnt[cc],mn[cc],mx[cc],last[cc] >> out;
		  }
		  if(detent=="1"){
		    off=last[best]-64;
		    printf "\n  >> CENTER of %s rests at %d (0x%02X). Ideal 64/0x40. Offset %+d.\n", id,last[best],last[best],off;
		    printf "[%s] CENTER resting=%d ideal=64 offset=%+d\n", id,last[best],off >> out;
		  }
		}'
	fi
	if [ -n "$notes" ]; then
		echo "  notes seen:"; echo "$notes" | sed 's/^/    /'
	fi
}

# args: id  "instruction"  detent(0/1)
capture() {
	local id="$1" instr="$2" detent="$3" mark slice
	c_hdr "$id"
	echo "$instr"
	read -r -p ">>> press ENTER to START capturing... " _ < "$TTY"
	mark=$(wc -l < "$LOG")
	c_ok ">>> MOVE IT NOW - events stream live - press ENTER when finished <<<"
	read -r _ < "$TTY"
	slice="$(mktemp)"
	tail -n +$((mark + 1)) "$LOG" > "$slice"
	summarize "$slice" "$id" "$detent"
	rm -f "$slice"
}

capture "crossfader" "Crossfader: full LEFT, full RIGHT, then SETTLE at the center detent and leave it there." 1
capture "ch1_fader"  "CH1 volume fader: run it 0 -> 10 -> 0 (all the way down, up, back down)." 0
capture "ch1_hi"     "CH1 HI (EQ) knob: full range (diagnostic - is the CH1 strip alive?)." 0
capture "ch1_trim"   "CH1 TRIM knob: full range (diagnostic)." 0
capture "ch2_fader"  "CH2 volume fader: run it 0 -> 10 -> 0." 0

c_hdr "DONE"
echo "Summary written to: $OUT"
c_dim "Tell Claude it's done."
