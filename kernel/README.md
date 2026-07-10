# DJM-T1 kernel ALSA driver

A standalone in-kernel ALSA driver for the Pioneer DJM-T1's built-in soundcard,
the upstream-track alternative to the userspace PipeWire driver in [`../audio/`](../audio/).
The mixer's audio interface (iface 0) is vendor-specific isochronous, not
USB-Audio-Class, so the generic `snd-usb-audio` never binds it; this module does,
and registers a real 6-in/6-out, 48 kHz, 24-bit ALSA card.

The device's frame layout (864-byte iso packets, 18 bytes/frame, S24_3LE 6-channel
interleaved) is exactly ALSA's `S24_3LE` 6-channel interleaved, so URB payloads are
copied to/from the PCM ring with no format conversion.

## Status

**Compiles cleanly (zero compiler warnings) and passes `scripts/checkpatch.pl`
with 0 errors / 0 warnings; NOT yet loaded or tested on hardware.** It is
assembled from the verified userspace wire format (`../audio/`), but the PCM/URB
pointer handling has not been validated against the device. Treat it as a
reviewed, statically-clean draft, not a proven driver. The tested audio path
today is the userspace PipeWire driver.

Loading an untested USB/ALSA module can oops or wedge the device. Review the source
first, and test on a machine you can reboot.

## Build

Match the kernel's toolchain. On a clang-built kernel (Arch, CachyOS, many others):

```sh
make LLVM=1        # this repo's dev kernel was clang-built
```

On a gcc-built kernel:

```sh
make
```

Needs the running kernel's build tree (`linux-headers` / `/lib/modules/$(uname -r)/build`).

## Try it (on a test machine)

```sh
sudo insmod djmt1_audio.ko
dmesg | tail                     # expect "Pioneer DJM-T1 ALSA card registered"
aplay -l ; arecord -l            # a new card should appear
# ... test playback/capture ...
sudo rmmod djmt1_audio
```

There is no conflict to resolve with `snd-usb-audio`: it does not bind iface 0
(vendor class 0xFF), so this module can own it directly.

## Scope

This driver covers **audio only**. MIDI on the DJM-T1 needs the arm plus the HID
gate (see the white paper) and is handled by the userspace bridge `../midi/djm_midi`;
folding that into the kernel is a separate, larger effort.
