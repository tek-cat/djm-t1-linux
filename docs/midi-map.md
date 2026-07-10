# DJM-T1 MIDI map

All messages are on **MIDI channel 1**. Continuous controls are 7-bit absolute
Control Change (`0xB0`); buttons are Note-On (`0x90`), `0x7F` on press / `0x00`
on release. Captured by running the bridge (`midi/djm_midi`) and watching its
ALSA port with `aseqdump -p "Pioneer DJM-T1"` while moving each control. (Once
`djm_midi` owns the MIDI interface, the raw `hw:X,0,0` ALSA-rawmidi node is gone;
the bridge's sequencer port replaces it.)

## Continuous controls (CC, status 0xB0)

| Control | Channel 1 | Channel 2 |
|---|---|---|
| TRIM / gain | CC 0x0D (13) | CC 0x0E (14) |
| HI EQ | CC 0x0F (15) | CC 0x10 (16) |
| MID EQ | CC 0x11 (17) | CC 0x12 (18) |
| LOW EQ | CC 0x13 (19) | CC 0x14 (20) |
| FILTER / COLOR | CC 0x09 (9) | CC 0x0B (11) |
| VOLUME fader | CC 0x15 (21) | CC 0x16 (22) |

| Master control | CC |
|---|---|
| Crossfader | CC 0x17 (23) |
| Headphone MIX (cue/master) | CC 0x18 (24) |
| Headphone LEVEL | analog only, sends no MIDI |

## Browse encoder

| Action | Message |
|---|---|
| Rotate | CC 0x00 (0), relative signed: `0x01` = +1 (CW), `0x7F` = -1 (CCW). Map with Mixxx `selectknob`. |
| Push | Note 0x00 (0) |

## Buttons (Note-On, status 0x90)

| Button | Note |
|---|---|
| Channel 1 CUE / PFL | 0x2F (47) |
| Channel 2 CUE / PFL | 0x30 (48) |
| LOAD, deck 1 | 0x01 (1) |
| LOAD, deck 2 | 0x02 (2) |

## LED / output feedback

`djm_midi` is bidirectional: its ALSA port is duplex, and any MIDI sent to it is
packed into USB-MIDI and written to the mixer's MIDI OUT (ep `0x04`, verified at
the USB level). To discover which message lights which LED, run the bridge and
send test notes/CCs to its port, then watch the panel. For example, put a Note-On
`0x2F` in a one-note MIDI file and play it to the port:

```sh
aplaymidi -p "Pioneer DJM-T1" note.mid    # watch the CH1 CUE LED
```

Then add `<output>` blocks to the Mixxx mapping that emit those messages on the
relevant Mixxx control changes. The CUE buttons are known to illuminate.

## Not yet mapped

The FX section and any remaining assignable buttons are not in the table yet:
capture them the same way (`aseqdump`, move/press, read) and extend
`mixxx/Pioneer-DJM-T1.midi.xml`. PRs welcome.

Note: the channel faders and EQ are also fed through the mixer's **analog** audio
path. In Mixxx use *internal* mixing (Mixxx does the mixing, these CCs drive its
software mixer) OR *external* mixing (the DJM mixes analog and you leave the
faders/EQ unmapped), not both, or you get double attenuation.
