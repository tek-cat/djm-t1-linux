# Pioneer DJM-T1 on Linux

Make the **Pioneer DJM-T1** mixer work as a MIDI controller on Linux (Mixxx, or anything that reads ALSA MIDI).

## The problem

Plug a DJM-T1 into Linux and it shows up as a MIDI device, but it stays **completely silent**. It works fine on Windows/macOS, so the hardware is fine. Two things are missing on Linux:

1. The DJM-T1 does not transmit MIDI until its proprietary Windows/macOS driver sends it a vendor-specific USB **"arm"** command on connect, and there is no such driver on Linux.
2. The arm alone is **not enough**. The mixer only streams its control surface while a full data session is live across several USB pipes at once, and specifically while its **HID interrupt endpoint is being actively polled** alongside a live audio session. It mirrors its control-surface data to both the MIDI and the HID endpoints in lockstep.

Stock Linux never opens the HID interface and never opens a PCM stream, so the mixer stays gated shut, and every knob and fader you touch sends nothing. Full story: [docs/WHITEPAPER.md](docs/WHITEPAPER.md).

## The solution

`djm_midi` is the fix: a small **libusb-to-ALSA bridge** that brings up the whole session the mixer wants. It claims the MIDI, HID and (optionally) audio interfaces, replays the arm, **polls the HID pipe to un-gate transmission**, and forwards the USB-MIDI stream into an ALSA sequencer port named **"Pioneer DJM-T1"** that Mixxx connects to like any class-compliant controller. With `--no-audio` it leaves the soundcard interface free for the separate PipeWire audio driver, so the DJM-T1 runs as **soundcard and MIDI controller at the same time**. The vendor command itself was reverse-engineered by capturing the Windows driver's USB traffic with `usbmon` (see [docs/reverse-engineering.md](docs/reverse-engineering.md)); `arm/djm_arm` is a minimal standalone reproduction of just that command. A ready-made **Mixxx mapping** is included.

## Quick start

```sh
git clone https://github.com/<you>/djm-t1-linux
cd djm-t1-linux
./install.sh    # builds + installs the MIDI bridge and audio driver, installs the
                # udev access rule and Mixxx mapping, and enables the user services
```

The bridge then auto-starts on login (a systemd user service running `djm_midi --no-audio`, paired with the PipeWire audio service so a live audio session is always up, which the mixer requires to transmit). Start Mixxx with the mixer connected, open Preferences → Controllers, select the **"Pioneer DJM-T1"** port, Enable it, and load the **"Pioneer DJM-T1"** mapping. To run it by hand instead of as a service, see **Manual usage** below.

## Manual usage (no install)

```sh
make -C midi
./midi/djm_midi -v              # bring up the full session; -v prints each MIDI message
# in another terminal:
aseqdump -p "Pioneer DJM-T1"    # watch the decoded MIDI; move a fader to see messages
```

Note: `djm_arm` plus `amidi` alone will **not** show MIDI. Arming without the HID pipe being polled (and without a live audio session) leaves the mixer gated shut; that is exactly what `djm_midi` handles.

## What's in the box

| Path | What |
|---|---|
| `midi/` | **the native MIDI bridge** (`djm_midi.c`): libusb to ALSA sequencer, arms + polls the HID gate; includes a per-user service |
| `arm/djm_arm.c` | standalone reproduction of just the vendor "arm" command |
| `mixxx/` | Mixxx controller mapping (`.midi.xml` + script) |
| `udev/99-djm-t1.rules` | non-root device access (also fires the legacy arm oneshot on plug-in) |
| `systemd/djm-t1-arm.service` | legacy arm-only oneshot (superseded by the `midi/` bridge service) |
| `audio/` | **the soundcard driver**: libusb streaming core + PipeWire device (6-in/6-out) |
| `tools/` | probe/capture scripts to extend the mapping (LEDs, more controls, HID) |
| `docs/WHITEPAPER.md` | full technical write-up |
| `docs/` | reverse-engineering method, MIDI map, USB protocol, next steps |

## Mapping coverage

Both channels' trim / 3-band EQ / filter (color) / volume fader, crossfader, headphone mix, the browse encoder (rotate + push), the PFL/cue buttons, and the load buttons. Everything transmits on MIDI channel 1. Full table: [docs/midi-map.md](docs/midi-map.md).

> **Deep dive:** [docs/WHITEPAPER.md](docs/WHITEPAPER.md) is a full technical write-up of the diagnosis, the capture method, and the findings.

## Status

- ✅ **MIDI**: **fully working natively and confirmed live in Mixxx.** The bridge (`midi/djm_midi.c`) replays the vendor arm and polls the HID endpoint to un-gate transmission; moving faders streamed 154+ Control Change messages on channel 1, verified three ways (the bridge's own decode, an independent `aseqdump`, and driving Mixxx). The crux: reproducing the arm alone is necessary but **not** sufficient. The mixer only transmits while its HID pipe is polled alongside a live audio session (full story in [docs/WHITEPAPER.md](docs/WHITEPAPER.md)). This is the first native-Linux MIDI the DJM-T1 has produced.
- ✅ **Simultaneous audio + MIDI**: confirmed. Run `djm_midi --no-audio` alongside the PipeWire audio driver and the DJM-T1 is a 6-in/6-out soundcard and a MIDI controller at the same time.
- ✅ **Mixxx mapping**: complete for the mixer section (faders, EQ, filter, crossfader, browse, cue, load).
- ✅ **Built-in soundcard (audio)**: **working** via a userspace **PipeWire** driver ([`audio/`](audio/)). The mixer appears as a 6-in / 6-out, 48 kHz, 24-bit device. The vendor-specific isochronous format was reverse-engineered ([docs/audio-plan.md](docs/audio-plan.md)) and is streamed directly with libusb. Verified on hardware: a PipeWire recording matches the raw USB capture channel-for-channel, with 0 USB transfer errors. A kernel ALSA driver (real ALSA card, upstreamable) is the planned next tier.
- 🔜 **Unified daemon**: fold audio + MIDI into one process that owns all four pipes (today they run as two cooperating processes that coexist via `--no-audio`).
- 🔜 **LED output feedback**: the cue buttons light up; mapping Mixxx state back to them is a straightforward next step (send MIDI to the device and watch which LEDs respond).

See [docs/NEXT-STEPS.md](docs/NEXT-STEPS.md) for how to tackle the open items, with ready-to-run probe scripts in [`tools/`](tools/).

## Troubleshooting

- **Still silent?** The mixer's armed/MIDI state is wiped by any USB re-enumeration, and it only transmits while `djm_midi` is running: arming with `djm_arm` alone is not enough (see the [white paper](docs/WHITEPAPER.md)). A flaky cable that keeps dropping the link will keep gating it shut, and can even wedge the device until a power-cycle. Use a known-good cable in a rear USB 2.0 port. Running `djm_midi` as a service re-establishes the session on every reconnect.
- **`djm_midi: not found / no access`**: install the udev rule (via `install.sh`) and make sure you're in the `audio` group (`groups | grep audio`), or run as root.

## How it was done

I ran the Windows driver in a VM with the mixer passed through, and captured the USB traffic **on the Linux host**. Because a passed-through device is proxied through the host kernel's usbfs, every packet the Windows driver sends is visible to `usbmon`, no analyzer needed inside Windows:

```
  ┌───────────┐   USB    ┌───────────────── Linux host ──────────────────┐
  │  DJM-T1   │ ───────► │   usbmon  ◄─── sees every URB on the bus        │
  │ 08e4:015e │          │      ▲                                          │
  └───────────┘          │      │  usbfs passthrough                       │
                         │   ┌──┴──────────── QEMU / KVM ───────────────┐  │
                         │   │  Windows 7 + Pioneer driver               │  │
                         │   │  (issues the vendor "arm" control writes) │  │
                         │   └───────────────────────────────────────────┘  │
                         └───────────────────────────────────────────────────┘
```

The arm turned out to be five vendor control writes. But arming alone left the mixer silent: diffing the *complete* working session showed it also needs its HID pipe polled and an audio session kept live. Both are handled by `midi/djm_midi.c`. Full method: [docs/reverse-engineering.md](docs/reverse-engineering.md). Full write-up: [docs/WHITEPAPER.md](docs/WHITEPAPER.md).

## License

MIT. See [LICENSE](LICENSE). Not affiliated with or endorsed by Pioneer DJ / AlphaTheta.
