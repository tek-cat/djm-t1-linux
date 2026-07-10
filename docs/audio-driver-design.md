# DJM-T1 audio driver design

Staged build of a Linux driver for the DJM-T1's built-in soundcard. Format is known
and fixed (see [audio-plan.md](audio-plan.md)): **48 kHz, 24-bit, 6-in / 6-out,
asynchronous isochronous**, interface 0 alt 1, EP `0x82` IN / `0x01` OUT, 864-byte
packets at 1 ms. Interface 0 has **no kernel driver bound**, so libusb can claim it
directly without detaching anything (MIDI on iface 2 and HID on iface 3 are
untouched).

## Phases

**Phase 1: userspace streaming core (`audio/`, C + libusb).** Validate the format,
clock, and channel map end to end before any kernel code.
- *1a Capture:* claim iface 0 / alt 1, run a ring of async iso IN transfers on
  `0x82`, de-interleave 6 x 24-bit, report per-packet framing and per-channel
  levels. Self-verifiable: error-free streaming, 864-byte packets, structured
  non-zero input.
- *1b Playback:* async iso OUT ring on `0x01`, push a generated tone; confirm
  error-free full-duplex. The device is the async clock master and both directions
  share it, so IN completions pace the OUT ring.
- *1c Channel map + quality:* feed a known signal into each mixer channel to learn
  which of the 6 IN channels is which, and confirm audio sounds correct. Needs a
  desk session.

**Phase 2: PipeWire integration.** Wrap the streaming core as a PipeWire node so
apps see a real 6-in/6-out device. The usable end product on modern systems.

**Phase 3: kernel ALSA driver.** Port the validated design to a
`snd-usb-caiaq`-style out-of-tree module: a real ALSA card, upstreamable.

## Streaming core architecture

- `djmt1_usb`: device open, claim iface 0, set alt 1, teardown.
- `djmt1_iso`: the iso engine, allocate N transfers x M packets, fill/submit,
  resubmit in the completion callback, run `libusb_handle_events`. One instance per
  direction (IN capture, OUT playback).
- `pcm`: de-interleave/interleave S24_3LE <-> planar float per 6 channels; ring
  buffers between the iso callbacks and the consumer/producer.
- Front ends: `djmt1-capture` (dump/analyze IN) for 1a; `djmt1-loopback`/tone for
  1b; later a PipeWire node for phase 2.

## Clocking

Async endpoints: the device clocks itself. No feedback endpoint is present; the IN
stream's steady 864-byte/ms arrival is the master clock. Capture is driven purely by
IN completions. For playback we keep the OUT ring one to two transfers ahead and let
the device drain it at its own rate; over/underruns are counted and surfaced.

## Access & build

- Device access: udev rule `08e4:015e MODE=0660 GROUP=audio` (installed; user is in
  `audio`). No root needed to run.
- Build: `make -C audio` (gcc + libusb-1.0). Later: PipeWire dev headers for phase 2,
  kernel headers for phase 3.

## Verification split (user is remote)

Autonomous (with the access rule): clean streaming, 864-byte framing, non-zero
structured input, clock stability (packet cadence), no XRUNs. Needs the desk: channel
identity mapping and subjective audio quality.

## Risks

- Async OUT pacing without a feedback endpoint may need tuning (buffer depth) to
  avoid drift; measured and adjusted empirically in 1b.
- 24-bit packing is assumed S24_3LE (3 bytes/sample); confirmed against real input in
  1a and, if needed, corrected.
