// Pioneer DJM-T1 mapping script for Mixxx
// Control numbers verified by full-panel calibration (tools/calibrate.py).
// All controls are on MIDI channel 1. Every button emits Note-On 0x7F then 0x00
// on a single push, so a direct mapping of a toggle control would switch it on
// then instantly back off. These helpers act on the press (0x7F) edge only.
// (cue_default is mapped directly instead, because it wants both edges.)

var DJMT1 = {};

DJMT1.init = function(id, debugging) {};

DJMT1.shutdown = function() {};

// Flip a binary channel control on the button-press edge. group comes from the
// <group> element in the XML (e.g. "[Channel1]").
DJMT1.toggle = function(group, key, value) {
    if (value === 0x7F) {
        engine.setValue(group, key, engine.getValue(group, key) ? 0 : 1);
    }
};

// Toggle a channel's headphone PFL on button press.
DJMT1.togglePFL = function(channel, control, value, status, group) {
    DJMT1.toggle(group, "pfl", value);
};

// Deck A/B PLAY-PAUSE toggle on button press.
DJMT1.togglePlay = function(channel, control, value, status, group) {
    DJMT1.toggle(group, "play", value);
};

// Deck A/B SYNC (sync_enabled) toggle on button press.
DJMT1.toggleSync = function(channel, control, value, status, group) {
    DJMT1.toggle(group, "sync_enabled", value);
};
