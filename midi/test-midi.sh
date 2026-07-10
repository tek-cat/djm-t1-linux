#!/bin/sh
# Quick confirmation that the DJM-T1 streams MIDI natively on Linux.
# Runs the bridge with -v so every decoded MIDI message prints; move any
# fader/knob and you should see lines appear. Ctrl-C to stop.
# SPDX-License-Identifier: MIT
set -e
DIR=$(dirname "$0")
[ -x "$DIR/djm_midi" ] || make -C "$DIR"
echo "Starting djm_midi -v ... move a fader or knob on the DJM-T1."
echo "(each control move should print a 'MIDI: ...' line; Ctrl-C to stop)"
echo
exec "$DIR/djm_midi" -v
