# Next steps: extending the DJM-T1 support

The core is solved: **MIDI works** (via `midi/djm_midi`, which arms the mixer,
polls the HID pipe to un-gate transmission, and bridges USB-MIDI to an ALSA
sequencer port) and the **built-in soundcard works** (via `audio/djmt1-pipewire`).
Run both together for the full mixer (`djm_midi --no-audio` + `djmt1-pipewire`,
or the two systemd user services). What's left is refinement and reach.

**Prereqs:** build/install (`make -C midi && make -C audio`), plug into a solid
USB 2.0 port, start the bridge (`midi/djm_midi`). Confirm MIDI flows:
`aseqdump -p "Pioneer DJM-T1"` then move a fader.

## 1. LED / output feedback  (low effort, mostly done)

`djm_midi` is bidirectional and the output path is **verified at the USB level**
(MIDI to its ALSA port is written to the mixer's MIDI OUT, ep `0x04`). Conventional
CUE-LED `<output>` blocks (notes `0x2F`/`0x30`) are already in
`mixxx/Pioneer-DJM-T1.midi.xml`. **Left:** confirm on the panel that those notes
light the CUE LEDs (send them with `aplaymidi -p "Pioneer DJM-T1"`), adjust if not,
and add `<output>` blocks for any other LEDs.

## 2. Finish the MIDI map  (low effort)

Map the FX section and any assignable buttons not yet covered. Run the bridge,
watch `aseqdump -p "Pioneer DJM-T1"`, move each unmapped control one at a time,
and add the new CC/Note numbers to [midi-map.md](midi-map.md) and the mapping.

## 3. 10-bit control resolution via HID  (medium effort)

The HID pipe carries every continuous control at **10-bit** resolution; MIDI only
publishes 7 bits (`MIDI = HID >> 3`). Full analysis and report structure:
[hid-analysis.md](hid-analysis.md). `djm_midi` already reads that pipe, so the plan
is to decode the 10-bit fields and emit **14-bit (high-resolution) MIDI** on the
same ALSA port, which Mixxx maps natively. **Left (one hardware capture):** move
each control one at a time to map which HID report/offset is which physical
control, then wire the field-to-high-res-CC table into the bridge.

## 4. Digital vinyl (DVS / timecode)  (config + bench check)

The mixer was built for Traktor Scratch; its phono inputs are digitized to the USB
input channels we already stream, so control vinyl can drive Mixxx's vinyl control.
Wiring and Mixxx configuration: [dvs-timecode.md](dvs-timecode.md). **Left:** confirm
the input-pair-to-deck map and that Mixxx locks onto the timecode.

## 5. Single unified daemon  (medium effort)

Today audio and MIDI are two processes on different interfaces (`djmt1-pipewire`
on iface 0; `djm_midi --no-audio` on ifaces 2+3), coordinated by the systemd
`After=`/`Wants=` ordering so a live audio session exists before the bridge arms.
A single daemon that owns all interfaces would remove that coupling and reduce
install to one service.

Concrete design (the pieces already exist):
- Base it on `audio/djmt1-pipewire.c`, which already pairs a PipeWire main loop
  with `djmt1_iso.c`'s libusb streaming thread via ring buffers.
- Add MIDI/HID/arm to that same libusb thread: claim ifaces 2 and 3, submit the
  ep `0x85` bulk read and ep `0x87` interrupt read, replay the arm (all lifted
  from `midi/djm_midi.c`).
- Set up the ALSA sequencer port in the main process; have the MIDI callback push
  decoded events to it, and poll seq input for LED output, as `djm_midi` does.
- One process owns all four pipes, so the gate's audio-session precondition is
  always met and there is nothing to order.

## 6. Kernel drivers  (large effort, upstreamable)

The audio path is a userspace PipeWire driver; a proper kernel ALSA driver (real
ALSA card) is the upstreamable next tier. See
[audio-driver-design.md](audio-driver-design.md).

---

Items 1, 3, and 4 each need one short capture at the mixer; the rest is software.
PRs welcome.
