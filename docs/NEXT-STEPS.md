# Next steps: extending the DJM-T1 support

The core is solved: **MIDI works** (via `midi/djm_midi`, which arms the mixer,
polls the HID pipe to un-gate transmission, and bridges USB-MIDI to an ALSA
sequencer port) and the **built-in soundcard works** (via `audio/djmt1-pipewire`).
Run both together for the full mixer (`djm_midi --no-audio` + `djmt1-pipewire`,
or the two systemd user services). What's left is refinement.

**Prereqs:** build/install (`make -C midi && make -C audio`), plug into a solid
USB 2.0 port, start the bridge (`midi/djm_midi`). Confirm MIDI flows:
`aseqdump -p "Pioneer DJM-T1"` then move a fader.

## 1. LED / output feedback  (low effort)

Goal: light the mixer's LEDs (cue buttons, etc.) from Mixxx state.

`djm_midi` is already bidirectional: MIDI sent to its ALSA port is written to the
mixer's MIDI OUT (ep `0x04`). Send test messages and watch the panel to learn
which message lights which LED (start with the cue notes `0x2F`/`0x30`):

```sh
aconnect <your-source> "Pioneer DJM-T1"   # or use aplaymidi to send notes/CCs
```

Then add matching `<output>` blocks to `mixxx/Pioneer-DJM-T1.midi.xml`.

## 2. Finish the MIDI map  (low effort)

Map the FX section and any assignable buttons not yet covered. Run the bridge,
watch `aseqdump -p "Pioneer DJM-T1"`, move each unmapped control one at a time,
and add the new CC/Note numbers to [midi-map.md](midi-map.md) and the mapping.

## 3. Single unified daemon  (medium effort)

Today audio and MIDI are two processes sharing the device on different interfaces
(`djmt1-pipewire` on iface 0; `djm_midi --no-audio` on ifaces 2+3). A single
daemon that owns all interfaces and exposes both the PipeWire audio device and the
ALSA MIDI port would remove the start-order coupling (MIDI needs a live audio
session) and simplify install to one service. The building blocks are in
`audio/djmt1_iso.c` and `midi/djm_midi.c`.

## 4. Kernel drivers  (large effort, upstreamable)

The audio path is a userspace PipeWire driver; a proper kernel ALSA driver (real
ALSA card) is the upstreamable next tier. See [audio-driver-design.md](audio-driver-design.md).

---

Results from 1 and 2 fold straight into the mapping and docs. PRs welcome.
