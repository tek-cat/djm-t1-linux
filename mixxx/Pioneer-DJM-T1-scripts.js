// Pioneer DJM-T1 mapping script for Mixxx
// Captured 2026-07-09. All controls are on MIDI channel 1.
// Only the headphone CUE (PFL) buttons need script logic: each physical push
// emits Note-On 0x7F immediately followed by 0x00, so a direct mapping would
// switch pfl on then instantly off. We toggle on the press (0x7F) edge instead.

var DJMT1 = {};

DJMT1.init = function(id, debugging) {};

DJMT1.shutdown = function() {};

// Toggle a channel's headphone PFL on button press.
// group is passed from the <group> element in the XML (e.g. "[Channel1]").
DJMT1.togglePFL = function(channel, control, value, status, group) {
    if (value === 0x7F) {
        var current = engine.getValue(group, "pfl");
        engine.setValue(group, "pfl", current ? 0 : 1);
    }
};
