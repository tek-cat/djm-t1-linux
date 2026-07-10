# Pioneer DJM-T1 on Linux

Native Linux support for the **Pioneer DJM-T1** DJ mixer: a working MIDI control surface and a 6-in / 6-out soundcard, with no vendor driver.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-1793d1)
![Language: C](https://img.shields.io/badge/language-C-555)
![MIDI + Audio: working](https://img.shields.io/badge/MIDI%20%2B%20audio-working-2ea44f)

The DJM-T1 is a Traktor-era mixer that only ever shipped Windows and macOS drivers. Plug it into Linux and it enumerates as a MIDI device but transmits nothing, and its built-in soundcard never appears. This project makes it work on Linux: the mixer's faders and knobs stream MIDI into Mixxx, and it runs as a 48 kHz / 24-bit audio interface at the same time. The vendor protocol was recovered by reverse-engineering the Windows driver's USB traffic; the full write-up is in [docs/WHITEPAPER.md](docs/WHITEPAPER.md).

## Contents

- [The problem](#the-problem)
- [The solution](#the-solution)
- [Quick start](#quick-start)
- [Manual usage](#manual-usage-no-install)
- [What's in the box](#whats-in-the-box)
- [Mapping coverage](#mapping-coverage)
- [Status](#status)
- [How it was done](#how-it-was-done)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [License](#license)

## The problem

Plug a DJM-T1 into Linux and it shows up as a MIDI device, but it stays **completely silent**. It works fine on Windows and macOS, so the hardware is healthy. Two things are missing on Linux:

1. The DJM-T1 does not transmit MIDI until its proprietary Windows/macOS driver sends it a vendor-specific USB **"arm"** command on connect, and there is no such driver on Linux.
2. The arm alone is **not enough**. The mixer only streams its control surface while a full data session is live across several USB pipes at once, and specifically while its **HID interrupt endpoint is being actively polled** alongside a live audio session. It mirrors its control-surface data to both the MIDI and the HID endpoints in lockstep.

Stock Linux never opens the HID interface and never opens a PCM stream, so the mixer stays gated shut, and every knob and fader you touch sends nothing. Full story: [docs/WHITEPAPER.md](docs/WHITEPAPER.md).

## The solution

`djm_midi` is the fix: a small **libusb-to-ALSA bridge** that brings up the whole session the mixer wants. It claims the MIDI, HID and (optionally) audio interfaces, replays the arm, **polls the HID pipe to un-gate transmission**, and forwards the USB-MIDI stream into an ALSA sequencer port named **"Pioneer DJM-T1"** that Mixxx connects to like any class-compliant controller. With `--no-audio` it leaves the soundcard interface free for the separate PipeWire audio driver, so the DJM-T1 runs as **soundcard and MIDI controller at the same time**.

The vendor command was reverse-engineered by capturing the Windows driver's USB traffic with `usbmon` (see [docs/reverse-engineering.md](docs/reverse-engineering.md)); `arm/djm_arm` is a minimal standalone reproduction of just that command. A ready-made **Mixxx mapping** is included.

## Quick start

```sh
git clone https://github.com/tek-cat/djm-t1-linux
cd djm-t1-linux
./install.sh    # builds + installs the MIDI bridge and audio driver, installs the
                # udev access rule and Mixxx mapping, and enables the user services
```

The bridge then auto-starts on login (a systemd user service running `djm_midi --no-audio`, paired with the PipeWire audio service so a live audio session is always up, which the mixer requires to transmit). Start Mixxx with the mixer connected, open **Preferences → Controllers**, select the **"Pioneer DJM-T1"** port, enable it, and load the **"Pioneer DJM-T1"** mapping. To run it by hand instead of as a service, see [Manual usage](#manual-usage-no-install).

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
| `midi/djm_midi.c` | **the native MIDI bridge**: libusb to ALSA sequencer, arms + polls the HID gate, bidirectional (LED out) |
| `midi/djm_full.c` | unified daemon: soundcard + MIDI controller in one process, all four USB pipes (drafted, not yet hardware-tested) |
| `midi/systemd/` | per-user services: `djm-midi.service` (the tested two-service setup) plus `djm-full.service` (the unified daemon, drafted) |
| `arm/djm_arm.c` | standalone reproduction of just the vendor "arm" command |
| `audio/` | **the soundcard driver** (`djmt1-pipewire`): libusb iso streaming core + PipeWire device (6-in/6-out), plus capture/monitor tools and its user service |
| `kernel/` | drafted in-kernel ALSA driver for the soundcard (compiles cleanly; not yet hardware-tested) |
| `mixxx/` | Mixxx controller mapping (`.midi.xml` + script) |
| `udev/99-djm-t1.rules` | non-root device access |
| `systemd/djm-t1-arm.service` | legacy arm-only oneshot (superseded by the `midi/` bridge service) |
| `docs/WHITEPAPER.md` | full technical write-up: diagnosis, capture method, findings |
| `docs/` | reverse-engineering method, MIDI map, USB protocol, HID analysis, DVS notes, next steps |
| `tools/` | probe/capture scripts to extend the mapping, plus `bench-check.sh` hardware validation |
| `install.sh` | one-shot build + install of everything above |

## Mapping coverage

Both channels' trim, 3-band EQ, filter (color) and volume fader, plus the crossfader, headphone mix, the browse encoder (rotate + push), the PFL/cue buttons, and the load buttons. Everything transmits on MIDI channel 1. Full table: [docs/midi-map.md](docs/midi-map.md).

## Status

| Capability | State |
|---|---|
| **MIDI control surface** | ✅ Working natively, confirmed live in Mixxx |
| **Built-in soundcard (audio)** | ✅ Working via userspace PipeWire driver, verified on hardware |
| **Simultaneous audio + MIDI** | ✅ Confirmed |
| **Mixxx mapping** | ✅ Complete for the mixer section |
| **Unified daemon** | 🔜 Drafted: `djm_full` folds both halves into one process and compiles; not yet run end-to-end on hardware |
| **Kernel audio driver** | 🔜 Drafted, compiles cleanly; needs hardware validation |
| **LED output feedback** | 🔜 Output path verified; panel check pending |
| **10-bit control resolution** | 🔜 HID pipe carries 10-bit values; exposing as 14-bit MIDI is a code change |
| **Digital vinyl (DVS / timecode)** | 🔜 Phono inputs already digitized to USB; can drive Mixxx vinyl control |

Details on each:

- ✅ **MIDI**: the bridge replays the vendor arm and polls the HID endpoint to un-gate transmission; moving faders streamed 154+ Control Change messages on channel 1, verified three ways (the bridge's own decode, an independent `aseqdump`, and driving Mixxx). The crux: reproducing the arm alone is necessary but **not** sufficient. The mixer only transmits while its HID pipe is polled alongside a live audio session (full story in [docs/WHITEPAPER.md](docs/WHITEPAPER.md)). This is the first native-Linux MIDI the DJM-T1 has produced.
- ✅ **Audio**: the mixer appears as a 6-in / 6-out, 48 kHz, 24-bit device via a userspace **PipeWire** driver ([`audio/`](audio/)). The vendor-specific isochronous format was reverse-engineered ([docs/audio-plan.md](docs/audio-plan.md)) and is streamed directly with libusb. Verified on hardware: a PipeWire recording matches the raw USB capture channel-for-channel, with 0 USB transfer errors.
- ✅ **Simultaneous audio + MIDI**: run `djm_midi --no-audio` alongside the PipeWire driver and the DJM-T1 is a soundcard and a MIDI controller at once. This two-service setup is the tested default.
- 🔜 **Unified daemon**: `midi/djm_full` folds the soundcard and the MIDI controller into one process (all four pipes, no start-order coordination). It reuses the two hardware-verified pieces unchanged and compiles cleanly, but has not yet been run end-to-end on the mixer; the two-service setup remains the tested default.
- 🔜 **Kernel audio driver**: a standalone in-kernel ALSA driver ([`kernel/`](kernel/)) is drafted and **compiles cleanly** (a real 6-in/6-out card, upstream-track); it still needs loading and validation on hardware.
- 🔜 **LED output feedback**: `djm_midi` is bidirectional and writing to the mixer's MIDI OUT (ep `0x04`) is verified; conventional CUE-LED `<output>` blocks are in the mapping, pending a panel check.
- 🔜 **10-bit control resolution**: the HID pipe carries every continuous control at 10-bit precision (MIDI exposes only 7). Since `djm_midi` already reads that pipe, exposing it as 14-bit MIDI is a code change, not new hardware. Analysis: [docs/hid-analysis.md](docs/hid-analysis.md).
- 🔜 **Digital vinyl (DVS / timecode)**: the mixer was built for Traktor Scratch; its phono inputs are digitized to the USB channels we already stream, so they can drive Mixxx's vinyl control. Wiring: [docs/dvs-timecode.md](docs/dvs-timecode.md).

See [docs/NEXT-STEPS.md](docs/NEXT-STEPS.md) for how to tackle the open items, with ready-to-run probe scripts in [`tools/`](tools/).

## How it was done

I ran the Windows driver in a VM with the mixer passed through, and captured the USB traffic **on the Linux host**. Because a passed-through device is proxied through the host kernel's usbfs, every packet the Windows driver sends is visible to `usbmon`, with no analyzer needed inside Windows:

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

The arm turned out to be five vendor control writes. But arming alone left the mixer silent: diffing the *complete* working session against the Linux attempts showed it also needs its HID pipe polled and an audio session kept live. Both are handled by `midi/djm_midi.c`. Full method: [docs/reverse-engineering.md](docs/reverse-engineering.md). Full write-up: [docs/WHITEPAPER.md](docs/WHITEPAPER.md).

## Troubleshooting

- **Still silent?** The mixer's armed/MIDI state is wiped by any USB re-enumeration, and it only transmits while `djm_midi` is running: arming with `djm_arm` alone is not enough (see the [white paper](docs/WHITEPAPER.md)). A flaky cable that keeps dropping the link will keep gating it shut, and can even wedge the device until a power-cycle. Use a known-good cable in a rear USB 2.0 port. Running `djm_midi` as a service re-establishes the session on every reconnect.
- **`djm_midi: not found` / no access**: install the udev rule (via `install.sh`) and make sure you are in the `audio` group (`groups | grep audio`), or run as root.

## Building from source

Build dependencies (development headers): `libusb-1.0`, `alsa-lib`, and `libpipewire-0.3`. On Arch-based systems: `pacman -S libusb alsa-lib pipewire`.

```sh
make -C arm                     # arm/djm_arm
make -C midi                    # midi/djm_midi (add: make -C midi djm_full)
make -C audio djmt1-pipewire    # the soundcard driver
make -C kernel LLVM=1           # optional: the in-kernel driver (LLVM=1 for clang-built kernels)
```

`install.sh` runs the userspace builds and installs everything. The kernel module is built separately and is not installed by `install.sh`.

## License

MIT. See [LICENSE](LICENSE). Not affiliated with or endorsed by Pioneer DJ / AlphaTheta.
