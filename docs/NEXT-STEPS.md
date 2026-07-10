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

## 5. Single unified daemon  (built)

`midi/djm_full.c` folds audio and MIDI into one process: it starts the `djmt1_iso`
audio engine (PipeWire, iface 0), then runs the MIDI/HID/arm bridge on a second
libusb context (ifaces 2+3) in a thread, so one process owns all four pipes and the
gate's audio-session precondition is always met (nothing to order). Build with
`make -C midi djm_full`; run `./midi/djm_full`; install with
`sudo make -C midi install-full` and `make -C midi install-full-service` (it
replaces the two split services). Verified to start and bring up both halves at
once (4 PipeWire nodes + the armed ALSA MIDI port, no crash); the live
fader-to-Mixxx path is the same one-capture confirmation as the split setup.

## 6. Device configuration from Linux  (reverse-engineering)

On Windows the Setting Utility configures the mixer (MIDI channel, and likely
crossfader/fader curves) via vendor control writes, the same class of command as
the arm. Capturing the utility while it changes each setting (same VM + usbmon
method as [reverse-engineering.md](reverse-engineering.md)) would let those
settings be changed from Linux with no Windows. Best done at the bench: it needs
the device passed to the VM, which re-enumerates it.

## 7. Kernel audio driver  (drafted, compiles; needs hardware validation)

`kernel/djmt1_audio.c` is a standalone in-kernel ALSA driver for the soundcard
(the upstream-track alternative to the userspace PipeWire driver). It builds
cleanly against the running kernel (`make -C kernel LLVM=1` on a clang-built
kernel) and registers a 6-in/6-out S24_3LE card; the device's frame layout maps
1:1 to `S24_3LE`, so no conversion. **It has not been loaded or tested on
hardware** yet: validate the PCM/URB pointer handling on a machine you can reboot,
then it is a candidate for upstreaming. Design notes:
[audio-driver-design.md](audio-driver-design.md); build/test steps:
[../kernel/README.md](../kernel/README.md).

---

Items 1, 3, and 4 each need one short capture at the mixer; the rest is software.
PRs welcome.
