// djm_arm - arm the Pioneer DJM-T1's MIDI mode on Linux.
//
// The DJM-T1 does not transmit MIDI until its (Windows/macOS-only) driver sends
// a short sequence of vendor USB control writes. This tool replays exactly that
// sequence, reverse-engineered by capturing the Windows driver's USB traffic
// with usbmon. After running it, the mixer streams MIDI like any class-compliant
// controller and can be used in Mixxx et al.
//
// Build:  make            (or: gcc -O2 -o djm_arm djm_arm.c -lusb-1.0)
// Run:    ./djm_arm       (needs write access to the USB node; see udev/ rule)
//
// SPDX-License-Identifier: MIT
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VID 0x08e4
#define PID 0x015e

static libusb_device_handle *h;
static int verbose = 1;

static int wr(unsigned wv, unsigned wi) {
    int r = libusb_control_transfer(h, 0x40, 0x03, wv, wi, NULL, 0, 1000);
    if (verbose)
        printf("  WRITE wValue=0x%04x wIndex=0x%04x -> %d %s\n",
               wv, wi, r, r == 0 ? "ok" : libusb_strerror(r));
    return r;
}
static int rd(unsigned wi) {
    unsigned char b[3] = {0, 0, 0};
    int r = libusb_control_transfer(h, 0xc0, 0x00, 0x0000, wi, b, 3, 1000);
    if (verbose)
        printf("  READ  wIndex=0x%04x -> %d [%02x %02x %02x]\n",
               wi, r, b[0], b[1], b[2]);
    return r;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-q")) verbose = 0;

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx)) { fprintf(stderr, "djm_arm: libusb_init failed\n"); return 1; }

    // Retry the open: on hotplug/udev the device may not be ready instantly.
    for (int tries = 0; tries < 20 && !h; tries++) {
        h = libusb_open_device_with_vid_pid(ctx, VID, PID);
        if (!h) usleep(100 * 1000);
    }
    if (!h) {
        fprintf(stderr, "djm_arm: DJM-T1 (%04x:%04x) not found or no access.\n"
                        "         Run as root, or install the udev rule (see udev/).\n", VID, PID);
        libusb_exit(ctx);
        return 2;
    }

    // Vendor-recipient control transfers: no need to claim interfaces or detach
    // the ALSA driver, so hw:X,0,0 stays available for amidi/Mixxx afterward.
    if (verbose) printf("djm_arm: arming DJM-T1 MIDI...\n");
    int ok = 1;
    ok &= wr(0x1100, 0x8002) == 0; rd(0x8002);
    ok &= wr(0x2200, 0x8002) == 0; rd(0x8002);
    ok &= wr(0x030a, 0x8002) == 0; rd(0x8002);
    ok &= wr(0x0000, 0x8003) == 0;
    ok &= wr(0x0000, 0x8004) == 0;

    libusb_close(h);
    libusb_exit(ctx);
    if (verbose) printf("djm_arm: %s\n", ok ? "armed." : "FAILED (a write errored).");
    return ok ? 0 : 3;
}
