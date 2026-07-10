# DJM-T1 MIDI map

All messages are on **MIDI channel 1**. Continuous controls are 7-bit absolute
Control Change (`0xB0`). Buttons are Note-On (`0x90`): a single push emits
velocity `0x7F` immediately followed by `0x00`, so a control that should toggle
is driven on the `0x7F` edge in the mapping script.

This map was produced by full-panel calibration: `tools/calibrate.py` (a curses
TUI) drives `aseqdump` against the running `midi/djm_midi` bridge and records
what each control emits. Raw results live in `tools/calibration.json`.

> **Number reuse:** the mixer reuses the same low numbers across message *types*.
> For example CC `0x0D` (Ch1 TRIM) and Note `0x0D` (FX1 ON) share a number but
> differ by status byte, so they never collide. Match on status **and** number.

## Mapped in Mixxx today

These are in `mixxx/Pioneer-DJM-T1.midi.xml` and are the verified, in-use set.

### Continuous controls (CC, status `0xB0`)

| Control | Channel 1 | Channel 2 |
|---|---|---|
| TRIM / gain | CC `0x0D` (13) | CC `0x0E` (14) |
| HI EQ | CC `0x0F` (15) | CC `0x10` (16) |
| MID EQ | CC `0x11` (17) | CC `0x12` (18) |
| LOW EQ | CC `0x13` (19) | CC `0x14` (20) |
| VOLUME fader | CC `0x15` (21) | CC `0x16` (22) |

| Master control | CC |
|---|---|
| Crossfader | CC `0x17` (23), and its mirror CC `0x18` (24), see note below |

### Browse encoder

| Action | Message |
|---|---|
| Rotate | CC `0x00` (0), relative signed: `0x01` = +1 (CW), `0x7F` = -1 (CCW). Mixxx `selectknob`. |
| Push | Note `0x00` (0) |

### Buttons (Note-On, status `0x90`)

| Button | Note | Mixxx |
|---|---|---|
| Ch1 CUE / PFL | `0x2F` (47) | `[Channel1] pfl` (script toggle) |
| Ch2 CUE / PFL | `0x30` (48) | `[Channel2] pfl` (script toggle) |
| LOAD deck A / B | `0x01` / `0x02` | `LoadSelectedTrack` |
| Deck A / B PLAY | `0x03` / `0x04` | `play` (script toggle) |
| Deck A / B CUE | `0x0B` / `0x0C` | `cue_default` |
| Deck A / B SYNC | `0x05` / `0x06` | `sync_enabled` (script toggle) |

## Captured but not yet mapped

Verified control numbers, left out of the mapping until the workflow is decided.
Extend the XML the same way once you choose Mixxx targets.

### FX sections (FX1 left, FX2 right)

| Control | FX1 | FX2 |
|---|---|---|
| DRY/WET | CC `0x01` (1) | CC `0x02` (2) |
| param 1 / 2 / 3 | CC `0x03` / `0x05` / `0x07` | CC `0x04` / `0x06` / `0x08` |
| ON (GROUP/SINGLE) | Note `0x0D` (13) | Note `0x0E` (14) |
| SELECT 1 / 2 / 3 | Note `0x0F` / `0x11` / `0x13` | Note `0x10` / `0x12` / `0x14` |
| FX DESIGN FX1 / FX2 (per deck) | Deck A `0x07` / `0x09` | Deck B `0x08` / `0x0A` |

### Auto-loop encoders (relative CC)

| | Deck A/C | Deck C/D |
|---|---|---|
| Rotate | CC `0x09` (9) | CC `0x0B` (11) |
| Push | Note `0x15` (21) | Note `0x17` (23) |
| SELECT (CROSS.F. CONTROL) | Note `0x19` (25) | Note `0x1A` (26) |

### Performance pads (HOT CUE, SAMPLER)

The four pad rows interleave in one consecutive block starting at Note `0x1B`.
For pad *N* (1-4), base = `0x1B + 4*(N-1)`, then:

| Pad | A/C HOT CUE | A/C SAMPLER | C/D HOT CUE | C/D SAMPLER |
|---|---|---|---|---|
| offset from base | +0 | +1 | +2 | +3 |
| 1 | `0x1B` (27) | `0x1C` (28) | `0x1D` (29) | `0x1E` (30) |
| 2 | `0x1F` (31) | `0x20` (32) | `0x21` (33) | `0x22` (34) |
| 3 | `0x23` (35) | `0x24` (36) | `0x25` (37) | `0x26` (38) |
| 4 | `0x27` (39) | `0x28` (40) | `0x29` (41) | `0x2A` (42) |

| Other | A/C | C/D |
|---|---|---|
| ACTIVE | Note `0x2C` (44) | Note `0x2E` (46) |
| PLAY MODE | Note `0x6C` (108) | Note `0x6E` (110) |

Global: SNAP / QUANTIZE = Note `0x31` (49).

> **SHIFT is a second layer, not yet fully mapped.** Holding SHIFT changes the
> codes the performance pads emit (an early pass saw the samplers move into the
> `0x5C`-`0x68` range). Those shifted codes are only partially discovered. The
> table above is the **base** (no-SHIFT) layer. A deliberate base-vs-SHIFT pass
> is needed before mapping the pads, so they are intentionally left unmapped.

## Analog / local controls (send no MIDI)

Confirmed silent by calibration, so they are correctly absent from the mapping:
MASTER LEVEL, BOOTH MONITOR, HEADPHONES LEVEL, HEADPHONES MIXING, crossfader
FEELING ADJ., CROSS F. REVERSE, UTILITY / WAKE UP, and SHIFT (SHIFT modifies
other buttons locally and emits nothing on its own).

The CD/PHONO/USB source switches are audio-routing selectors (Ch1 on CC `0x6B`
with `0x65`/`0x71`; Ch2 on CC `0x68` with `0x6E`/`0x74`) and are not Mixxx controls.

## Corrections from calibration

Two bindings in the pre-calibration map were wrong and have been removed:

- **FILTER / COLOR on CC `0x09` / `0x0B`** was incorrect: those CCs are the Deck
  A/C and C/D **auto-loop encoders**. The DJM-T1 has no color/filter knob.
- **Headphone MIX on CC `0x18`** was incorrect: CC `0x18` is the **crossfader's
  mirror** signal (the crossfader emits CC `0x17` and CC `0x18` together). The
  headphone-mix knob is analog and sends no MIDI, so mapping it to CC `0x18`
  meant the crossfader dragged Mixxx's headphone mix. Use CC `0x17` for the
  crossfader and leave headphone mix unmapped.

## Open calibration items

- **Center offsets.** The crossfader and EQ knobs need a careful pass that ends
  at the physical center detent, so the true halfway point (not MIDI 64) can be
  recorded and the mapping can remap it to 0.5. See `tools/calibrate.py` (detented
  controls prompt for a center); the current center values are not yet reliable.
- **SHIFT layer.** Map the shifted pad/button codes (see the SHIFT note above).

## LED / output feedback

`djm_midi` is bidirectional: its ALSA port is duplex, and MIDI sent to it is
packed into USB-MIDI and written to the mixer's MIDI OUT (ep `0x04`, verified at
the USB level). The CUE-button LEDs (`0x2F` / `0x30`) are wired as `<output>`
blocks and are known to illuminate. To map more LEDs, send the button's own note
to the port and watch the panel:

```sh
aplaymidi -p "Pioneer DJM-T1" note.mid    # watch which LED lights
```

Note: the channel faders and EQ are also fed through the mixer's **analog** audio
path. In Mixxx use *internal* mixing (Mixxx mixes; these CCs drive its software
mixer) OR *external* mixing (the DJM mixes analog and you leave the faders/EQ
unmapped), not both, or you get double attenuation.
