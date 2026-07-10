# The DJM-T1's built-in soundcard on Linux

The mixer's audio is a vendor-specific interface with no kernel driver. This
documents the wire format (captured, below) and the driver path. **Status: done.**
The format below is streamed by the userspace PipeWire driver in [`../audio/`](../audio/),
which exposes the mixer as a 6-in/6-out device; a kernel ALSA driver is the planned
next tier ([audio-driver-design.md](audio-driver-design.md)).

## The interface

Interface 0 is **vendor-specific** (class `0xFF`), not USB Audio Class, so the
generic `snd-usb-audio` driver ignores it. Alt setting 1 exposes two isochronous
endpoints:

| Endpoint | Dir | Type | wMaxPacketSize | bInterval |
|---|---|---|---|---|
| `0x01` | OUT | iso, asynchronous | 1024 | 4 (= 1 ms at high speed) |
| `0x82` | IN | iso, asynchronous | 1024 | 4 (= 1 ms) |

(The device is Pioneer-vendor `08e4`, so the NI `snd-usb-caiaq` driver, which
handles NI `17cc` vendor-iso gear, does not apply.)

## Captured wire format

Captured with `usbmon` on the Linux host while the passed-through device streamed
a test tone in the Windows VM (the Pioneer driver exposes the DJM as the default
WDM playback device, so any audio routes through these iso endpoints). Every iso
packet, both directions, was **exactly 864 bytes**, one per 1 ms interval, with no
variation:

```
C Zi:1:002:2 ... 0:0:864 0:1024:864 0:2048:864 ...   (IN,  each packet 864 B)
C Zo:1:002:1 ... 0:0:864 0:864:864  0:1728:864 ...   (OUT, each packet 864 B)
```

864 bytes/ms is a constant (non-varying) size, so the rate is integer-frames-per-ms:
**48000 Hz** (48 frames/ms). 864 / 48 = **18 bytes/frame = 6 channels x 24-bit**.

**Format: 48 kHz, 24-bit, 6 in / 6 out, asynchronous isochronous.** Same both
directions. Streaming is enabled purely by `SET_INTERFACE(iface 0, alt 1)`; stopping
and restarting playback produced **no** control transfers, i.e. the format is
**fixed** with no rate/format handshake to reverse-engineer.

## Driver path (much simpler than first feared)

Because the format is fixed and there is no vendor control handshake, a driver only
needs to: claim interface 0, select alt 1, and pump 48 kHz / 24-bit / 6-channel iso
URBs (864-byte packets) on `0x82` IN and `0x01` OUT.

- **ALSA kernel driver.** A small driver modeled on `snd-usb-caiaq` (which does
  exactly this for NI's vendor-iso devices), registering a fixed 6-in/6-out
  48k/S24 PCM. Cleanest for users.
- **Userspace (fastest to prototype).** libusb submitting the iso transfers,
  bridged into PipeWire/JACK. Higher latency, no kernel work; good for validating
  the format end to end before committing to a kernel driver.

Remaining unknowns are small: exact channel mapping (which of the 6 is which
deck/aux/timecode) and clock/feedback behavior of the async endpoint. Both fall out
of a prototype that plays known signals and inspects the returned frames.

## Reproduce the capture

1. VM with the device passed through, Pioneer driver installed (see
   [reverse-engineering.md](reverse-engineering.md)).
2. Play audio through the DJM (it is the default WDM output; a plain PCM WAV works).
3. On the host: `cat /sys/kernel/debug/usb/usbmon/1u` and read the `Z` (iso) lines;
   the `status:offset:length` triplets give the per-packet byte count.
