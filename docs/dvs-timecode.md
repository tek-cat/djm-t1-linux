# Digital vinyl (DVS / timecode) on the DJM-T1 with Mixxx

The "T" in DJM-T1 is Traktor: the mixer was built as the front end of a Traktor
Scratch digital-vinyl setup. Its built-in soundcard digitizes the phono/line
inputs, so control vinyl (or control CD) can drive a software deck. Now that the
soundcard works on Linux ([`../audio/`](../audio/)), that same path can drive
**Mixxx's vinyl control**. This documents the wiring and what still needs a bench
check.

## How DVS works here

```
 control record  ->  turntable  ->  DJM-T1 phono in  ->  built-in ADC  ->  USB in
                                                                             |
                                       Mixxx vinyl control (xwax decoder)  <-+
                                       decodes timecode -> deck position/speed
```

The timecode (a tone + noise pattern pressed onto control vinyl) reaches Mixxx as
ordinary audio on the mixer's USB input channels. Mixxx's built-in vinyl-control
decoder turns it into scratch/position/pitch for the assigned deck. No extra
hardware beyond the control records the setup already uses with Traktor.

## What we already have

The PipeWire driver exposes the DJM-T1 as a 6-in / 6-out device. From the capture
work ([../audio/README.md](../audio/README.md)): **input channels 0/1 and 4/5
carry signal**, 2/3 read near-silent unconnected, and the USB output loops back
onto input 4/5 (a master/monitor return). So the deck phono inputs are among
0/1 and 4/5; the exact deck-to-channel-pair assignment is the one thing to confirm
on the bench (play a record on deck 1, see which input pair moves).

## Mixxx setup

1. Run the audio service so the mixer is a PipeWire device (`djmt1-pipewire`).
2. Mixxx, Preferences, Sound Hardware:
   - Set **Vinyl Control In 1** to the DJM input pair feeding deck 1 (and In 2 for
     deck 2). Pick the pair from 0/1 and 4/5 confirmed above.
3. Mixxx, Preferences, Vinyl Control:
   - Enable vinyl control; pick the **timecode type** matching your control records.
     Mixxx supports Serato CV02, **Traktor MK1/MK2**, and MixVibes. DJM-T1 users
     coming from Traktor will most likely have Traktor control vinyl, which Mixxx
     decodes directly.
   - Set lead-in, and choose absolute (position) or relative (scratch-only) mode.
4. Enable vinyl control on a deck (the deck's vinyl button, or `g` / passthrough
   controls) and drop the needle.

## What needs a bench check (hardware-gated)

- **Channel-to-deck map:** which USB input pair (0/1 vs 4/5) is deck 1's phono vs
  deck 2's vs the master return. Play a record on one deck and watch the per-channel
  levels (`audio/djmt1-capture` prints them) to pin it down.
- **Signal level / phono stage:** confirm the inputs deliver a clean, correctly
  leveled timecode signal to Mixxx (the mixer's phono preamp should already be in
  the path; verify the decoder locks and shows a steady quality reading).
- **Timecode type:** confirm which of Mixxx's supported formats your control
  records use and that Mixxx locks onto them.

If the input pairs are clean and the timecode is a format Mixxx supports, this is a
configuration task, not new code: the audio path that makes DVS possible is already
built and verified. This is the DJM-T1's original purpose, running on Linux.
