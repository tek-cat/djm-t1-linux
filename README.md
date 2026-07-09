# Pioneer DJM-T1 on Linux

Make the **Pioneer DJM-T1** mixer work as a MIDI controller on Linux (Mixxx, or anything that reads ALSA MIDI).

## The problem

Plug a DJM-T1 into Linux and it shows up as a MIDI device, but it stays **completely silent**. It works fine on Windows/macOS, so the hardware is fine. The reason: the DJM-T1 does not transmit MIDI until its **proprietary Windows/macOS driver sends it a vendor-specific USB "arm" command** on connect. There is no such driver on Linux, so the mixer is never told to start, and every knob and fader you touch sends nothing.

This project provides the missing piece.

## The solution

`djm_arm` is a tiny libusb tool that replays the exact arm command the Windows driver sends (reverse-engineered by capturing the driver's USB traffic with `usbmon`; see [docs/reverse-engineering.md](docs/reverse-engineering.md)). Run it once after plugging in the mixer, or let the included udev rule auto-run it, and the DJM-T1 streams MIDI like any class-compliant controller. A ready-made **Mixxx mapping** is included.

## Quick start

```sh
git clone https://github.com/<you>/djm-t1-linux
cd djm-t1-linux
./install.sh
```

Then unplug/replug the mixer (it auto-arms), start Mixxx with it connected, and load the **"Pioneer DJM-T1"** mapping under Preferences → Controllers.

## Manual usage (no install)

```sh
make -C arm
sudo ./arm/djm_arm            # arm the mixer (or run without sudo once the udev rule is in)
amidi -l                      # find the DJM's hw address, e.g. hw:1,0,0
amidi -p hw:1,0,0 -d          # watch MIDI; now move a fader and you'll see bytes
```

## What's in the box

| Path | What |
|---|---|
| `arm/djm_arm.c` | the arming tool (build with `make`) |
| `mixxx/` | Mixxx controller mapping (`.midi.xml` + script) |
| `udev/99-djm-t1.rules` | non-root access + **auto-arm on plug-in** |
| `systemd/djm-t1-arm.service` | oneshot service the udev rule triggers |
| `tools/` | probe/capture scripts to extend the mapping (LEDs, more controls, HID) |
| `docs/WHITEPAPER.md` | full technical write-up |
| `docs/` | reverse-engineering method, MIDI map, USB protocol, next steps |

## Mapping coverage

Both channels' trim / 3-band EQ / filter (color) / volume fader, crossfader, headphone mix, the browse encoder (rotate + push), the PFL/cue buttons, and the load buttons. Everything transmits on MIDI channel 1. Full table: [docs/midi-map.md](docs/midi-map.md).

> **Deep dive:** [docs/WHITEPAPER.md](docs/WHITEPAPER.md) is a full technical write-up of the diagnosis, the capture method, and the findings.

## Status

- ✅ **MIDI arm**: reverse-engineered and reproduced natively. Verified at the protocol level, the mixer returns the exact same acknowledgements to our arm as it does to the Windows driver. End-to-end stream confirmation is a hardware check away (the mixer only emits MIDI when a control is physically moved, on Windows too).
- ✅ **Mixxx mapping**: complete for the mixer section (faders, EQ, filter, crossfader, browse, cue, load).
- 🔬 **Built-in soundcard (audio)**: not implemented, but now **characterized**. The audio is a vendor-specific (non-USB-Audio-Class) isochronous interface, so there is no PCM device. Capturing it while it streamed revealed a **fixed 48 kHz / 24-bit / 6-in-6-out async isochronous format**, enabled by a single `SET_INTERFACE` with no rate handshake, which makes a driver tractable. Format and driver path: [docs/audio-plan.md](docs/audio-plan.md). Until a driver exists, use a separate audio interface.
- 🔜 **LED output feedback**: the cue buttons light up; mapping Mixxx state back to them is a straightforward next step (send MIDI to the device and watch which LEDs respond).

See [docs/NEXT-STEPS.md](docs/NEXT-STEPS.md) for how to tackle the open items, with ready-to-run probe scripts in [`tools/`](tools/).

## Troubleshooting

- **Still silent after arming?** The DJM-T1's MIDI state is wiped by any USB re-enumeration. A flaky cable that keeps dropping will keep un-arming it (and can even wedge the device until a power-cycle). Use a known-good cable in a rear USB 2.0 port, and re-run `djm_arm` after any disconnect. The auto-arm udev rule handles this for you.
- **`djm_arm: not found or no access`**: run as root, or make sure the udev rule is installed and you're in the `audio` group (`groups | grep audio`).

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

The arm turned out to be five vendor control writes, reproduced natively in ~60 lines of libusb. Full method: [docs/reverse-engineering.md](docs/reverse-engineering.md). Full write-up: [docs/WHITEPAPER.md](docs/WHITEPAPER.md).

## License

MIT. See [LICENSE](LICENSE). Not affiliated with or endorsed by Pioneer DJ / AlphaTheta.
