# Reviving a Windows-only DJ mixer on Linux by reverse-engineering its USB "arm" command

**Author:** Aaron Landis
**Date:** July 2026
**Subject:** Pioneer DJM-T1 (USB `08e4:015e`)

## Abstract

The Pioneer DJM-T1 is a DJ mixer that doubles as a MIDI control surface. On Windows and macOS it works; on Linux it enumerates as a MIDI device but transmits nothing, so every fader and knob is dead. This paper documents how I found the cause (the vendor's driver silently sends the mixer a proprietary "arm" command on connect, and there is no Linux driver to send it), how I recovered that command by capturing the Windows driver's USB traffic through a virtual machine using `usbmon`, and how I reproduced it natively on Linux in about sixty lines of C. The device accepts the reproduced command with byte-identical acknowledgements to the Windows driver. The result is a small tool plus a Mixxx mapping that make the mixer usable on Linux. The capture technique generalizes to any "works on Windows, dead on Linux" USB peripheral.

## 1. Background

The DJM-T1 presents four USB interfaces: a vendor-specific isochronous audio interface (its built-in Traktor Scratch soundcard), an audio-control interface, a standard USB-MIDI Streaming interface, and an HID interface. On Linux the generic `snd-usb-audio` driver binds the MIDI interface and creates an ALSA MIDI port. Everything looks correct. But moving any control produces no MIDI: the kernel's received-byte counter for the endpoint stays at zero.

The decisive clue is that the same hardware and cable work on Windows. That rules out the cable, the port, permissions, and the DJ software, and points at the one thing that differs between the platforms: the driver.

## 2. Diagnosis

Working through the stack from the outside in eliminated the usual suspects:

- **Not the application.** Mixxx enumerates the controller and opens its MIDI input; the kernel confirms the port is opened. No data arrives.
- **Not permissions or the sound server.** The device is a native package install with full device access; PipeWire and the ALSA sequencer are wired correctly.
- **Not the USB link, once stabilized.** A separate hardware issue (a marginal cable causing the device to re-enumerate every 30 to 60 seconds) was resolved first; it is unrelated to the silence but worth noting because any re-enumeration resets the device's MIDI state.
- **The device simply is not transmitting.** With the MIDI endpoint open by a listener, `/proc/asound/card*/midi0` shows `Rx bytes : 0`. Nothing is coming out of the hardware.

Per the vendor's documentation, the MIDI channel is configured through a Windows/macOS "Setting Utility," and the driver package is required. That framed the hypothesis: **the driver puts the mixer into a MIDI-transmitting mode with a command Linux never sends.**

## 3. The capture strategy: usbmon through a passthrough VM

The obvious way to see what the Windows driver sends is a USB analyzer inside Windows. There is a cleaner path. If Windows runs in a virtual machine with the device passed through, QEMU accesses that device through the host kernel's usbfs. Every packet the Windows driver exchanges then traverses the Linux host's USB stack, where `usbmon` can observe it. The capture is taken on Linux, exactly where the analysis and the eventual reimplementation happen, with no analyzer software in the guest.

The setup:

1. A Windows 7 guest under KVM (Microsoft's freely distributed "IE on Win7" evaluation image). Windows 7 has no xHCI driver, so the VM is given a **USB 2.0 (EHCI)** controller and an i440fx machine with an IDE boot disk.
2. The mixer is passed through by vendor and product ID via a libvirt `<hostdev>` entry.
3. Pioneer's DJM-T1 driver and Setting Utility are installed in the guest. Notably, the Setting Utility's MIDI tab only selects the channel; the arming is done by the driver itself on bind.
4. On the host, `usbmon` is loaded and its debugfs stream for the mixer's bus is made readable.

To isolate the arm, the device is detached from the VM back to the host, the capture is started, and the device is reattached so the Windows driver freshly binds it. The bind produces a compact, self-contained trace.

## 4. Findings

Filtering the standard enumeration (descriptor reads, `SET_INTERFACE`) out of the trace leaves a short run of **vendor control transfers** that the Linux generic driver never issues. These are the arm. Each is a control write with `bmRequestType = 0x40` (vendor, host-to-device, device recipient) and `bRequest = 0x03`, interleaved with read-backs:

```
WRITE  bRequest=0x03  wValue=0x1100  wIndex=0x8002       READ wIndex=0x8002 -> 00 01 00
WRITE  bRequest=0x03  wValue=0x2200  wIndex=0x8002       READ wIndex=0x8002 -> 00 01 00
WRITE  bRequest=0x03  wValue=0x030a  wIndex=0x8002       READ wIndex=0x8002 -> 00 01 00
WRITE  bRequest=0x03  wValue=0x0000  wIndex=0x8003
WRITE  bRequest=0x03  wValue=0x0000  wIndex=0x8004
```

The `wIndex` values `0x8002`, `0x8003`, and `0x8004` behave like device registers; `wValue` is the value written. The read-backs (`bmRequestType = 0xC0`) return `00 01 00`, a status acknowledgement. Immediately after these writes, the Windows driver posts a bulk-IN read on the MIDI endpoint, that is, it starts listening for the MIDI the mixer will now produce. Everything else in the trace is either standard enumeration or the separate setup of the isochronous audio interface, which is not needed for MIDI.

## 5. Native reimplementation and verification

The reproduction is direct. Using libusb, open the device and issue each transfer with `libusb_control_transfer`. Because the requests target the device (not an interface), no interface claim or kernel-driver detach is required, and the ALSA MIDI port remains available to `amidi` and Mixxx afterward:

```c
libusb_control_transfer(h, 0x40, 0x03, 0x1100, 0x8002, NULL, 0, 1000);
libusb_control_transfer(h, 0xc0, 0x00, 0x0000, 0x8002, buf, 3, 1000); // -> 00 01 00
// ...three more writes...
```

**Verification.** Running the tool on the Linux host, every write returns status `0` and every read returns `00 01 00`, byte-for-byte identical to the responses the mixer gave the Windows driver. The device accepts the natively-issued arm exactly as it accepts the vendor's. This establishes correctness at the protocol layer.

One honest caveat about scope: the mixer emits MIDI only when a physical control is moved (this is true on Windows as well; opening the port alone yields no data). Confirming the resulting MIDI stream end-to-end therefore requires a hand on the hardware, the final field validation. The reverse-engineering itself, the identification and faithful reproduction of the arm, is complete and verified.

## 6. Productionizing

Two pieces make this usable day to day:

- **Auto-arm.** A udev rule grants the user non-root access to the device and, on plug-in, triggers a systemd oneshot that runs the arm tool. The armed state is volatile (any USB re-enumeration clears it), so re-arming automatically on every connect is the correct behavior.
- **Mixxx mapping.** The full control surface was mapped by arming the device and reading `amidi` while moving each control: both channels' trim, three-band EQ, filter, and volume; the crossfader; headphone mix; the browse encoder (a relative encoder) with its push; the cue and load buttons. All controls transmit on MIDI channel 1.

## 7. Limitations and future work

- **Built-in soundcard.** The DJM-T1's audio is a vendor-specific isochronous interface, not USB-Audio-Class, so Linux exposes no PCM device. Bringing it up would mean reverse-engineering the isochronous streaming setup (format, sample-rate control, packet layout) the same way and writing an ALSA or userspace driver. This is the largest remaining opportunity.
- **LED feedback.** The cue buttons illuminate. Mapping Mixxx state back to them is a small extension, discoverable on Linux by sending MIDI to the device and observing which messages light which LEDs.
- **HID interface.** The mixer's fourth interface is HID; whether it carries anything the MIDI interface does not is an open question worth a capture.

## 8. Reproducibility

The method is not specific to this mixer. For any USB device that works on Windows but is inert on Linux:

1. Run Windows in a VM and pass the device through with libvirt `<hostdev>`.
2. Capture on the **host** with `usbmon` while the Windows driver binds or exercises the device.
3. Diff the trace against what the Linux driver does; the vendor-specific transfers that only Windows sends are the missing initialization.
4. Reproduce them with libusb.

The tooling, the Mixxx mapping, and step-by-step reproduction notes are in the accompanying repository.

## Appendix: device summary

| Interface | Class | Endpoints | Role |
|---|---|---|---|
| 0 | Vendor-specific | iso `0x01` OUT / `0x82` IN (alt 1) | Built-in soundcard (no Linux driver) |
| 1 | Audio control | none | Control interface (no ALSA controls) |
| 2 | USB-MIDI | bulk `0x04` OUT / `0x85` IN | MIDI (works once armed) |
| 3 | HID | int `0x06` OUT / `0x87` IN | HID |
