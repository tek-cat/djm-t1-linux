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
| `docs/` | how it was reverse-engineered, the full MIDI map, the USB protocol |

## Mapping coverage

Both channels' trim / 3-band EQ / filter (color) / volume fader, crossfader, headphone mix, the browse encoder (rotate + push), the PFL/cue buttons, and the load buttons. Everything transmits on MIDI channel 1. Full table: [docs/midi-map.md](docs/midi-map.md).

## Status

- ✅ **MIDI**: working (arm + mapping).
- ⛔ **Built-in soundcard (audio)**: not yet. The DJM-T1's audio is a *vendor-specific isochronous* USB interface (not USB-Audio-Class), so the generic Linux driver ignores it and there is no PCM device. Reverse-engineering it into an ALSA driver is a possible follow-up — notes in [docs/protocol.md](docs/protocol.md). Until then, use a separate audio interface.
- 🔜 **LED output feedback**: the cue buttons light up; mapping Mixxx state back to them is a straightforward next step (send MIDI to the device and watch which LEDs respond).

## Troubleshooting

- **Still silent after arming?** The DJM-T1's MIDI state is wiped by any USB re-enumeration. A flaky cable that keeps dropping will keep un-arming it (and can even wedge the device until a power-cycle). Use a known-good cable in a rear USB 2.0 port, and re-run `djm_arm` after any disconnect. The auto-arm udev rule handles this for you.
- **`djm_arm: not found or no access`**: run as root, or make sure the udev rule is installed and you're in the `audio` group (`groups | grep audio`).

## How it was done

Booted Windows 7 in a KVM VM, passed the mixer through over USB, installed Pioneer's driver, and captured the USB traffic on the Linux host with `usbmon` while the driver bound the device. The arm turned out to be five vendor control writes. Full write-up: [docs/reverse-engineering.md](docs/reverse-engineering.md).

## License

MIT. See [LICENSE](LICENSE). Not affiliated with or endorsed by Pioneer DJ / AlphaTheta.
