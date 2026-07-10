# Reviving a Windows-only DJ mixer on Linux: reverse-engineering the vendor arm command and the hidden multi-pipe MIDI gate

**Author:** Aaron Landis
**Date:** July 2026
**Subject:** Pioneer DJM-T1 (USB `08e4:015e`)

## Abstract

The Pioneer DJM-T1 is a DJ mixer that doubles as a MIDI control surface. On Windows and macOS it works; on Linux it enumerates as a MIDI device but transmits nothing, so every fader and knob is dead. This paper documents the full cause and the working fix. The vendor's driver silently sends the mixer a proprietary "arm" command on connect, which I recovered by capturing the Windows driver's USB traffic through a passthrough virtual machine with `usbmon` and reproduced on Linux with byte-identical acknowledgements. But the arm turned out to be necessary, not sufficient: armed, with the MIDI input open, the mixer still emitted nothing. Diffing a complete working Windows session against the Linux attempts exposed the real gate. The DJM-T1 only transmits its control surface while a full data session is live across several USB pipes at once, and specifically while its HID interrupt endpoint is being actively polled alongside a live audio session; the mixer mirrors its control-surface data to both the MIDI and the HID endpoints in lockstep. Stock Linux opens neither the HID interface nor a PCM stream, so the mixer stays gated shut. The fix is `djm_midi`, a single libusb-to-ALSA-sequencer bridge that claims the MIDI, HID and (optionally) audio interfaces, replays the arm, polls the HID pipe to un-gate transmission, and forwards the USB-MIDI stream into an ALSA sequencer port. It is confirmed end to end: moving the faders now streams real Control Change messages into Mixxx, the first native-Linux MIDI this mixer has produced. With one flag it leaves the audio interface free for the repository's separate PipeWire soundcard driver, so the DJM-T1 works as soundcard and MIDI controller on Linux at the same time. The capture technique generalizes to any "works on Windows, dead on Linux" USB peripheral.

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

### 4.1 The arm: vendor control transfers

Filtering the standard enumeration (descriptor reads, `SET_INTERFACE`) out of the trace leaves a short run of **vendor control transfers** that the Linux generic driver never issues. These are the arm. Each is a control write with `bmRequestType = 0x40` (vendor, host-to-device, device recipient) and `bRequest = 0x03`, interleaved with read-backs:

```
WRITE  bRequest=0x03  wValue=0x1100  wIndex=0x8002       READ wIndex=0x8002 -> 00 01 00
WRITE  bRequest=0x03  wValue=0x2200  wIndex=0x8002       READ wIndex=0x8002 -> 00 01 00
WRITE  bRequest=0x03  wValue=0x030a  wIndex=0x8002       READ wIndex=0x8002 -> 00 01 00
WRITE  bRequest=0x03  wValue=0x0000  wIndex=0x8003
WRITE  bRequest=0x03  wValue=0x0000  wIndex=0x8004
```

The `wIndex` values `0x8002`, `0x8003`, and `0x8004` behave like device registers; `wValue` is the value written. The read-backs (`bmRequestType = 0xC0`) return `00 01 00`, a status acknowledgement. Reproduced natively with libusb, every write returns status `0` and every read returns `00 01 00`, byte-for-byte identical to the responses the mixer gave the Windows driver. Because these requests target the device (not an interface), no interface claim is needed to issue them. This establishes the arm as correct at the protocol layer.

### 4.2 The arm is necessary, not sufficient

Here is the turn the first pass of this work missed. Arming the device is required, but it does not, by itself, make the mixer talk. Armed on Linux, with the ALSA MIDI input open and a bulk-IN read pending on the MIDI endpoint (`0x85`), moving a fader produced **zero** MIDI bytes. The acknowledgements were perfect and the stream was still dead. Something beyond the arm gates transmission, and reproducing the arm alone (the earlier conclusion of this paper) does not revive the mixer.

### 4.3 The hidden gate: a live multi-pipe session and HID polling

The answer came from diffing the *complete* working Windows session, captured end to end with `usbmon` over the passthrough, rather than just the arm at connect. The working session keeps **four USB pipes live at once**:

- isochronous audio **IN** (`0x82`) and **OUT** (`0x01`), the built-in soundcard;
- the USB-MIDI bulk **IN** (`0x85`), the MIDI we want;
- the HID interrupt **IN** (`0x87`).

The decisive detail: the mixer **mirrors its control-surface data to both the MIDI endpoint (`0x85`) and the HID endpoint (`0x87`) in lockstep.** On stock Linux neither condition is ever met. Nothing opens the HID interface, so `usbhid` never polls `0x87`, and nothing opens a PCM stream, so the audio interface stays idle. The mixer stays gated shut.

I confirmed the gate with a single libusb owner replicating the Windows session and changing one variable at a time:

| Session state | MIDI out |
|---|---|
| armed + MIDI read pending | silent |
| armed + MIDI read pending + live isochronous audio session | silent |
| armed + MIDI read pending + live audio + HID interrupt (`0x87`) polled | **flows** |

The conclusion, stated as this paper's key finding: **the DJM-T1 gates its control-surface MIDI on its HID pipe being actively polled, together with a live audio session.** Polling the HID endpoint is precisely what the stock Linux stack never does, and it is the missing trigger the arm alone cannot supply.

One red herring was ruled out along the way. The Windows driver also sends a USB-Audio `SET_SAMPLING_FREQ = 48000` class request on the audio-IN endpoint (`bmRequestType = 0x22`, `SET_CUR`). Isolating it showed it is **not** the trigger: a live audio session from any source un-gates the mixer whether or not that specific request is replayed. The bridge still issues it when it owns the audio interface, purely to mirror Windows, but it is not part of the gate.

## 5. Verification: confirmed end to end and in Mixxx

The earlier version of this work could only claim correctness at the protocol layer and noted that confirming the MIDI stream end to end was "a hand on the hardware away." That field validation has now been done, and it passes.

With `djm_midi` running (Section 6), moving the mixer's faders and knobs streamed **154+ real Control Change messages on MIDI channel 1** in a single short session. The volume faders report on CC `0x15` and `0x16`, the crossfader on `0x17`, the headphone mix on `0x18`, and the trim, three-band EQ, filter and browse encoder each on their own controller number (the full table is in `docs/midi-map.md`). This was verified three independent ways:

1. the bridge's own decode (`djm_midi -v` prints each message as it is bridged);
2. an independent `aseqdump` subscribed to the bridge's ALSA sequencer port; and
3. **live in Mixxx**: the mixer now drives Mixxx's controls directly.

This is the first native-Linux MIDI the DJM-T1 has produced. The reverse-engineering and the working driver are complete, not just the protocol reproduction.

## 6. Productionizing: the djm_midi bridge

The fix ships as a single tool, `midi/djm_midi.c`, a libusb-to-ALSA-sequencer bridge that turns the multi-pipe discovery into something you run once and forget:

- **It brings up the full session.** It claims the MIDI interface (iface 2) and the HID interface (iface 3), and optionally the audio interface (iface 0), replays the arm, then **polls the HID interrupt endpoint** to un-gate transmission, exactly the condition Section 4.3 identified.
- **It presents an ordinary MIDI port.** It decodes the USB-MIDI event packets arriving on `0x85` (each is a 4-byte packet whose code-index number selects how many raw MIDI bytes follow) and forwards the recovered MIDI into an ALSA sequencer port named **"Pioneer DJM-T1"**. Mixxx, `aseqdump`, or anything that reads ALSA sequencer MIDI connects to that port; nothing downstream knows the arming and HID poll happened underneath.
- **It coexists with the soundcard.** With `--no-audio`, `djm_midi` leaves the audio interface (iface 0) unclaimed, so the repository's separate PipeWire soundcard driver (`djmt1-pipewire`, in `audio/`) owns it instead. That PipeWire stream supplies the live audio session the gate requires. This is confirmed live: **MIDI streaming and 6-channel audio at the same time**, the DJM-T1 working as both soundcard and MIDI controller on Linux simultaneously.
- **It re-establishes the session on every connect.** A udev rule grants non-root device access, and a per-user systemd service runs the bridge persistently. The armed and un-gated state is volatile (any USB re-enumeration clears it), so a service that restarts and re-arms on every plug-in is the correct model.
- **Mixxx mapping.** The full control surface was mapped by moving each control and reading the port: both channels' trim, three-band EQ, filter and volume; the crossfader; headphone mix; the browse encoder (a relative encoder) with its push; the cue and load buttons. All controls transmit on MIDI channel 1.

## 7. Limitations and future work

- **HID interface: resolved.** In the earlier write-up, whether the HID interface carried anything the MIDI interface did not was "an open question worth a capture." It is now answered: the HID pipe is the transmission **gate**. Actively polling it, together with a live audio session, is what makes the mixer stream MIDI at all. It behaves less like an alternate data path and more like an interlock.
- **Built-in soundcard: solved.** The DJM-T1's audio is a vendor-specific isochronous interface, not USB-Audio-Class. It is now brought up by a userspace PipeWire driver (`audio/`, a 6-in/6-out, 48 kHz, 24-bit device), documented separately in this repository. It is no longer an open problem.
- **A single unified daemon.** `djm_midi` and `djmt1-pipewire` currently run as two cooperating processes that each open the device (coexisting cleanly via `--no-audio`). Folding audio and MIDI into one process that owns all four pipes would remove the coordination between them and guarantee the gate's audio-session precondition is always met.
- **LED output feedback.** The cue buttons illuminate. Mapping Mixxx state back to them is a small extension, discoverable on Linux by sending MIDI to the device and observing which messages light which LEDs.

## 8. Reproducibility

The method is not specific to this mixer. For any USB device that works on Windows but is inert on Linux:

1. Run Windows in a VM and pass the device through with libvirt `<hostdev>`.
2. Capture on the **host** with `usbmon` while the Windows driver binds or exercises the device.
3. Diff the trace against what the Linux driver does; the vendor-specific transfers that only Windows sends are the missing initialization. Capture the *complete working session*, not just the connect: as this device showed, the missing piece can be a pipe that must stay open, not only a command that must be sent.
4. Reproduce them with libusb.

The tooling, the Mixxx mapping, and step-by-step reproduction notes are in the accompanying repository.

## Appendix: device summary

| Interface | Class | Endpoints | Role |
|---|---|---|---|
| 0 | Vendor-specific | iso `0x01` OUT / `0x82` IN (alt 1) | Built-in soundcard (userspace PipeWire driver, `audio/`) |
| 1 | Audio control | none | Control interface (no ALSA controls) |
| 2 | USB-MIDI | bulk `0x04` OUT / `0x85` IN | MIDI in (streams only during the full multi-pipe session; see 4.3) |
| 3 | HID | int `0x06` OUT / `0x87` IN | HID; polling `0x87` is the MIDI transmission gate (see 4.3) |
