# DJM-T1 HID interface: a 10-bit resolution channel

The DJM-T1's fourth interface (iface 3, HID, interrupt IN on ep `0x87`) is not
just the MIDI transmission gate ([WHITEPAPER.md](WHITEPAPER.md) section 4.3). It
also carries the mixer's continuous controls at **higher resolution than MIDI**.

## The finding: HID is 10-bit, MIDI is 7-bit

The mixer mirrors each control to both pipes in lockstep. Capturing a single fader
sweep (the control MIDI reports as CC `0x16`) and lining up the two streams:

| MIDI (7-bit) | HID field (LE 16-bit) | HID decimal |
|---|---|---|
| 0x20 = 32  | `02 01` | 258  |
| 0x2f = 47  | `7d 01` | 381  |
| 0x47 = 71  | `38 02` | 568  |
| 0x64 = 100 | `24 03` | 804  |
| 0x7c = 124 | `e2 03` | 994  |
| 0x7f = 127 | `fa 03` | 1018 |

HID value is almost exactly `MIDI * 8`, and the maximum observed (`0x03fa` = 1018)
sits just under 1023. So the ADC behind each control is **10-bit (0..1023)**, MIDI
simply publishes the top 7 bits (`MIDI = HID >> 3`). The HID pipe exposes the full
10 bits: **8x finer resolution**, three extra bits, on every continuous control.

For faders, the color/filter knob, and EQ, that is a real feel improvement (no
audible stepping on slow moves), and it is available for free: `djm_midi` already
polls ep `0x87` to un-gate MIDI, so it is reading these reports already.

## Report structure

The HID input is a multi-report layout. Each 16-byte report begins with a report
ID and packs several 10-bit little-endian fields:

| Report ID (byte 0) | Notes |
|---|---|
| `0x01`, `0x02` | byte 2 = `0x80` set; distinct control group |
| `0x03` | fields near the end of the report (bytes 14..15 seen moving) |
| `0x05`, `0x06` | further control groups |
| `0x09` | at least two fields: bytes [4,5] and [6,7], each 10-bit LE |

Example (report `0x09`, one fader moving at bytes [6,7] while [4,5] hold `0x02d3`):

```
09 00 00 00  d3 02 02 01  00 00 00 00  00 00 00 00
09 00 00 00  d3 02 7d 01  ...                          (same fader, higher)
09 00 00 00  d3 02 fa 03  ...                          (near full scale, 1018)
```

The complete field-to-control assignment (which report/offset is which physical
fader, EQ band, filter, crossfader, headphone mix) still needs a **per-control
capture**. The bridge has a `--hid-probe` mode that prints each HID report as it
changes, so this is one command plus moving controls:

```sh
djm_midi --hid-probe --no-audio -v    # prints "HID: .." on change and "MIDI: .." per message
#   move ONE control at a time; note which report ID + byte offset moves,
#   alongside the MIDI CC it corresponds to
```

That is the one hardware step left to finish this map.

## Using the extra resolution: high-res MIDI out of djm_midi

Because `djm_midi` already reads ep `0x87`, the clean way to expose 10-bit control
to Mixxx is to have the bridge decode the HID fields and emit **14-bit MIDI** for
them (a CC pair: MSB on the base controller, LSB on controller+0x20, the standard
high-resolution CC convention), or NRPN. Mixxx maps 14-bit CCs natively, so the
faders and filter would arrive at 10-bit precision through the same ALSA port,
with no separate HID device for Mixxx to open (which it could not anyway, since
`djm_midi` owns iface 3).

Design sketch for the bridge:

1. Keep the existing HID interrupt read on ep `0x87` (already there for the gate).
2. Parse each report by ID into its 10-bit fields.
3. Track per-field last value; on change, look up the field's assigned high-res
   CC and emit MSB (`value >> 7`) and LSB (`value & 0x7f`) on channel 1.
4. The 7-bit MIDI stream on ep `0x85` stays as the low-res fallback/compat path.

This needs the field-to-control map above to be filled in first. Until then the
7-bit MIDI path is complete and working; this is a precision upgrade on top.
