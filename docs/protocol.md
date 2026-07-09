# DJM-T1 USB protocol notes

USB ID **`08e4:015e`** (Pioneer Corp. "PIONEER DJM-T1"), high-speed (USB 2.0).

## Interfaces (1 configuration, 4 interfaces)

| # | Class | Endpoints | Purpose |
|---|---|---|---|
| 0 | Vendor-specific (0xFF) | alt 0: none; **alt 1**: EP `0x01` OUT + `0x82` IN, **isochronous** | Built-in soundcard (Traktor Scratch audio). Vendor protocol, *not* USB-Audio-Class → no Linux PCM. |
| 1 | Audio / Control | none | Control interface. Exposes **zero** ALSA mixer controls. |
| 2 | Audio / MIDI Streaming | EP `0x04` OUT (bulk), `0x85` IN (bulk) | **MIDI.** Standard USB-MIDI class, single alt setting. Works with the generic driver once the device is armed. |
| 3 | HID | EP `0x06` OUT (interrupt), `0x87` IN (interrupt) | HID interface (shows in Mixxx as "PIONEER DJM-T1 _3"). Not needed for MIDI mapping. |

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
verbatim; the device responds identically to Linux and to Windows.

The Windows driver additionally does `SET_INTERFACE(iface 0, alt 1)` to enable the
isochronous **audio** endpoints and `SET_IDLE` on the HID interface — neither is
needed to get MIDI.

**Volatility:** the armed state is lost on any USB re-enumeration (unplug, hub
reset, cable drop) and on power-cycle. Re-run `djm_arm` after any of those; the
udev rule automates it.

## Audio (unsolved)

Interface 0 is vendor-specific isochronous, so there is no Linux driver and no PCM
node. Making the built-in soundcard work would mean reverse-engineering the iso
streaming setup (audio format/altsetting, sample-rate control, packet layout) from
the Windows driver — capture `SET_INTERFACE`/vendor control traffic plus the iso
data with usbmon while Windows streams audio, then write an ALSA (snd-usb quirk or
standalone) or userspace driver. Contributions welcome. Until then, use a separate
audio interface.

## Reproducing / extending the capture

See [reverse-engineering.md](reverse-engineering.md).
