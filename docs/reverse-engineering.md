# How the arm command was reverse-engineered

The DJM-T1 works on Windows and stays silent on Linux, which points at the driver
doing something on connect that Linux doesn't. The goal was to see exactly what the
Windows driver sends the device, then replay it. This is a general recipe that works
for any "works on Windows, dead on Linux" USB device.

## The trick: capture on the Linux host, through a VM

Rather than install a USB analyzer inside Windows, run Windows in a VM with the
device **passed through**, and capture on the **Linux host**. QEMU forwards a
passed-through USB device via the host kernel's usbfs, so every packet the Windows
driver exchanges with the device is visible to Linux's `usbmon`, no guest-side
tooling, and the capture lands right where you'll analyze it.

## Steps

1. **VM.** A Windows 7 KVM guest (Microsoft's free "IE11 on Win7" image works).
   Give it a **USB 2.0 (EHCI)** controller (Win7 has no xHCI driver), i440fx
   machine, IDE boot disk. Pass the mixer through:
   ```xml
   <hostdev mode='subsystem' type='usb' managed='yes'>
     <source><vendor id='0x08e4'/><product id='0x015e'/></source>
   </hostdev>
   ```
2. **Driver.** Install Pioneer's DJM-T1 driver + Setting Utility in the guest. The
   utility's MIDI tab only sets the channel (CH 1); the *driver* does the arming.
3. **usbmon.** On the host:
   ```sh
   sudo modprobe usbmon                       # built-in on some kernels
   sudo chmod 755 /sys/kernel/debug /sys/kernel/debug/usb /sys/kernel/debug/usb/usbmon
   sudo chmod 644 /sys/kernel/debug/usb/usbmon/1u   # bus 1 = the mixer's bus
   ```
4. **Capture the bind.** Detach the device to the host, start the capture, then
   attach it to the VM so the Windows driver freshly binds it:
   ```sh
   virsh detach-device win7 djm-hostdev.xml
   timeout 25 cat /sys/kernel/debug/usb/usbmon/1u > windows_arm.txt &
   virsh attach-device win7 djm-hostdev.xml
   ```
5. **Read the trace.** usbmon "u" format: `<tag> <ts> <S|C> <type><dir>:<bus>:<dev>:<ep> ...`.
   Filter out the standard enumeration (descriptor GETs, `SET_INTERFACE`) and the
   **vendor** transfers jump out: control writes with `bmRequestType = 0x40`,
   `bRequest = 0x03`. Those, and nothing the Linux generic driver sends, are the arm.
   The device even posts a Bulk-IN read on the MIDI endpoint (`Bi:...:5`) right after.

## Replay on Linux

libusb, ~60 lines ([../arm/djm_arm.c](../arm/djm_arm.c)): open `08e4:015e`, then
`libusb_control_transfer(h, 0x40, 0x03, wValue, wIndex, NULL, 0, ...)` for each
write. Vendor-**device** recipient, so no interface claim or driver detach is needed.
The device returns the same `00 01 00` acks it gave Windows, confirmation the replay
is faithful.

But arming alone does **not** make the mixer transmit MIDI: capturing the *complete*
Windows session (not just the connect) showed it also gates transmission on its HID
pipe being polled alongside a live audio session. That is why the production tool is
[`../midi/djm_midi.c`](../midi/djm_midi.c) (arm + HID poll + audio + ALSA-seq bridge),
not `djm_arm` alone. Full story: [WHITEPAPER.md](WHITEPAPER.md).

## Gotchas we hit

- The armed state is **volatile**: any USB re-enumeration (or a flaky cable) wipes
  it. A drop-prone cable will keep un-arming the mixer and can wedge it until a
  power-cycle. Diagnose flapping with `journalctl -k | grep "usb 1-.*: USB disconnect"`.
- usbmon is a **kernel module matching the running kernel**. If you updated the
  kernel but haven't rebooted, its module dir is gone and `modprobe usbmon` fails
  with "not found", so reboot into the current kernel first.
- Passed-through USB devices still appear in host `lsusb` (QEMU claims them via
  usbfs, they stay on the bus). Check `/proc/asound/cards` to tell which side
  actually owns the ALSA/driver binding.
