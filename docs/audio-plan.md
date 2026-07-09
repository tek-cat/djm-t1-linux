# Plan: bring the DJM-T1's built-in soundcard to Linux

This is the largest unsolved piece and a separate, bigger effort than the MIDI arm.
It is scoped here so it can be picked up cleanly.

## The obstacle

Interface 0 of the DJM-T1 is **vendor-specific and isochronous** (class `0xFF`,
alt 1 has iso endpoints `0x01` OUT and `0x82` IN), not USB Audio Class. Linux has
no driver that recognizes it, so there is no PCM device and the mixer's four-in /
four-out Traktor Scratch soundcard is unusable. (Note: it is a Pioneer-vendor
device, `08e4`, so the Native Instruments `snd-usb-caiaq` driver, which handles NI
`17cc` gear, does not apply.)

## Capture plan (same method as the MIDI arm)

1. In the Win7 passthrough VM, install the driver and open the Setting Utility's
   ASIO panel, or play audio through the device with an ASIO host, so the driver
   actively streams.
2. On the Linux host, capture with usbmon on the mixer's bus. Isochronous traffic
   is high-volume, so keep captures short and stream a known signal (a fixed sine
   tone) to make the sample format legible in the bytes.
3. Extract two things from the trace:
   - **Setup:** `SET_INTERFACE(iface 0, alt 1)` plus any vendor control writes that
     select sample rate / format (look for `bmRequestType=0x40` transfers around the
     stream start, analogous to the MIDI arm's `0x8002`-family writes).
   - **Stream:** the iso packet size and cadence on `0x82` IN / `0x01` OUT, from
     which the sample rate, channel count, and sample width can be inferred.

## Analysis

- Confirm channels (expected 4 in / 4 out), sample rate (44.1/48k), and sample
  width (likely 24-bit packed or 32-bit) from packet size = channels x width x
  (rate / 8000) per microframe at high speed.
- Determine whether the format is close enough to UAC to be driven by an
  `snd-usb-audio` quirk/`QUIRK_AUDIO_FIXED_ENDPOINT`, or whether it needs a
  dedicated driver.

## Implementation options

- **ALSA kernel driver / quirk.** Cleanest for users. Either a `snd-usb-audio`
  quirk table entry if the descriptors can be coerced, or a small standalone driver
  modeled on `snd-usb-caiaq` (which does exactly this for NI's vendor-iso devices).
- **Userspace driver.** libusb reading/writing the iso endpoints, bridged into
  PipeWire/JACK. Faster to prototype, no kernel work, higher latency.

## Effort

Realistically multiple sessions: a capture pass, format reverse-engineering, then a
driver. Tracked as future work; MIDI (this repo's focus) does not depend on it. In
the meantime, use a separate USB audio interface.
