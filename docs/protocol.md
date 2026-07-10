# DJM-T1 USB protocol notes

USB ID **`08e4:015e`** (Pioneer Corp. "PIONEER DJM-T1"), high-speed (USB 2.0).

## Interfaces (1 configuration, 4 interfaces)

| # | Class | Endpoints | Purpose |
|---|---|---|---|
| 0 | Vendor-specific (0xFF) | alt 0: none; **alt 1**: EP `0x01` OUT + `0x82` IN, **isochronous** | Built-in soundcard. Vendor protocol, not USB-Audio-Class, so no kernel PCM; brought up by the userspace PipeWire driver in [`../audio/`](../audio/). |
| 1 | Audio / Control | none | Control interface. Exposes **zero** ALSA mixer controls. |
| 2 | Audio / MIDI Streaming | EP `0x04` OUT (bulk), `0x85` IN (bulk) | **MIDI.** Standard USB-MIDI class, single alt setting. But it only streams during the full multi-pipe session (see the gate below), not from arming alone. |
| 3 | HID | EP `0x06` OUT (interrupt), `0x87` IN (interrupt) | HID interface. **Polling `0x87` is the MIDI transmission gate:** the mixer mirrors its control surface to both the MIDI (`0x85`) and HID (`0x87`) pipes, and will not transmit unless the HID pipe is being read. |

## The MIDI "arm" sequence

The device sends no MIDI until it receives these **vendor control writes**
(`bmRequestType = 0x40`, `bRequest = 0x03`, `wLength = 0`), which the Windows
driver issues once on bind:

```
SETUP 40 03 wValue=0x1100 wIndex=0x8002   (then GET 0xC0 0x00 wIndex=0x8002, len 3 -> 00 01 00)
SETUP 40 03 wValue=0x2200 wIndex=0x8002   (GET -> 00 01 00)
SETUP 40 03 wValue=0x030a wIndex=0x8002   (GET -> 00 01 00)
SETUP 40 03 wValue=0x0000 wIndex=0x8003
SETUP 40 03 wValue=0x0000 wIndex=0x8004
```

`wIndex` `0x8002/0x8003/0x8004` look like device registers; `wValue` is the value
written. The `0xC0` reads return `00 01 00` (a status/ack). `djm_arm` replays this
verbatim; the device responds identically on Linux and on Windows.

## The gate: arming is necessary, not sufficient

Arming alone leaves the mixer silent on Linux, even with the MIDI input open and a
bulk-IN read pending on `0x85`. The Windows driver keeps a **full session** live:
`SET_INTERFACE(iface 0, alt 1)` plus isochronous audio streaming, the MIDI bulk-IN
read on `0x85`, and (crucially) an interrupt read on the HID endpoint `0x87`. The
mixer only transmits its control surface while its **HID pipe is polled alongside a
live audio session**. The `djm_midi` bridge ([`../midi/`](../midi/)) sets up all of
it. (The driver also sends a USB-Audio `SET_SAMPLING_FREQ = 48000` on `0x82`; that
one turned out not to be part of the gate.) Full analysis:
[WHITEPAPER.md](WHITEPAPER.md).

**Volatility:** the armed/session state is lost on any USB re-enumeration (unplug,
hub reset, cable drop) and on power-cycle, so `djm_midi` re-establishes it on every
connect (run it as the provided systemd service).

## Audio (solved)

Interface 0 is vendor-specific isochronous, so no kernel driver binds it. The format
was reverse-engineered (48 kHz, 24-bit, 6-in/6-out; see
[audio-plan.md](audio-plan.md)) and is streamed directly with libusb, exposed to
PipeWire as a 6-in/6-out device by [`../audio/djmt1-pipewire`](../audio/). A kernel
ALSA driver (upstreamable) is the planned next tier; see
[audio-driver-design.md](audio-driver-design.md).

## Reproducing / extending the capture

See [reverse-engineering.md](reverse-engineering.md).
