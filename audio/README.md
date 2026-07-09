# DJM-T1 audio (Linux)

Makes the Pioneer DJM-T1's **built-in soundcard** work on Linux. The mixer's audio
is a vendor-specific isochronous USB interface (not USB-Audio-Class), so no kernel
driver touches it. This is a userspace driver that streams it directly with libusb
and exposes it to **PipeWire** as a real **6-in / 6-out, 48 kHz, 24-bit** device.

Format was reverse-engineered (see [../docs/audio-plan.md](../docs/audio-plan.md));
design in [../docs/audio-driver-design.md](../docs/audio-driver-design.md).

## Build

```sh
make            # CLI tools (djmt1-capture, djmt1-playback, djmt1-monitor)
make djmt1-pipewire   # the PipeWire device (needs libpipewire-0.3 dev)
```

Needs `libusb-1.0`, `libpipewire-0.3`, and access to the device (be in the `audio`
group with the udev rule installed; see [../udev/](../udev/)).

## Use it

```sh
./djmt1-pipewire        # DJM-T1 appears as a PipeWire Source + Sink; Ctrl+C to stop
```

Then in `wpctl status` / `pavucontrol` you'll see **Pioneer DJM-T1** as both a
capture source and a playback sink. Route audio to/from it like any device.

Auto-start it as a user service:

```sh
sudo make install         # binaries -> /usr/local/bin
make install-service      # user service -> ~/.config/systemd/user
systemctl --user daemon-reload
systemctl --user enable --now djmt1-audio
```

## Validate / debug (no PipeWire)

```sh
./djmt1-capture 5 -o cap.s24   # stream the input, print per-channel levels + framing
./djmt1-playback 4 440 -18     # play a 440 Hz tone (-18 dBFS) out all channels
./djmt1-monitor 5 --tone       # exercise the full-duplex engine, print health
```

## Channels

Six channels each way (three stereo pairs). Observed: input ch0/1 and ch4/5 carry
signal, ch2/3 read near-silent with nothing connected, and the USB **output loops
back onto input ch4/5** (a master/monitor return). Exact deck/phono/master identity
is being mapped; contributions welcome.

## Status

Working: 6-in/6-out capture and playback through PipeWire, verified on hardware
(recording matches the raw USB capture channel-for-channel, 0 USB transfer errors).
Idle overrun/underrun counters are expected (no active stream); they stay flat under
real use. A kernel ALSA driver (real ALSA card, upstreamable) is the planned next
tier. See the design doc.
