// djmt1-playback - Phase 1b of the DJM-T1 audio driver.
// Claims interface 0 / alt 1 and streams a sine tone out the isochronous OUT
// endpoint (0x01) on all 6 channels. Verifies error-free playback streaming.
// (Audible confirmation and channel mapping are a desk task; the level is kept low.)
//
// Build: make djmt1-playback    Run: ./djmt1-playback [seconds] [freq_hz] [dbfs]
// SPDX-License-Identifier: MIT
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <math.h>

#define VID 0x08e4
#define PID 0x015e
#define IFACE 0
#define ALT 1
#define EP_OUT 0x01
#define PKT 864              /* bytes per iso OUT packet = 48 frames x 18 bytes */
#define FRAMES_PER_PKT (PKT / (6 * 3))   /* 48 */
#define NPKT 32
#define NXFR 8
#define CH 6

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

static double phase = 0.0, freq = 440.0, amp = 0.06;   /* ~ -24 dBFS default */
static const double SR = 48000.0;
static uint64_t out_pkts = 0, out_err = 0, xfr_errs = 0;
static int inflight = 0;

static void fill_tone(unsigned char *buf, int npkts) {
    for (int pk = 0; pk < npkts; pk++) {
        unsigned char *pb = buf + pk * PKT;
        for (int f = 0; f < FRAMES_PER_PKT; f++) {
            double s = amp * sin(phase);
            phase += 2.0 * M_PI * freq / SR;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            int32_t v = (int32_t)(s * 8388607.0);
            for (int c = 0; c < CH; c++) {
                unsigned char *p = pb + (f * CH + c) * 3;
                p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF;
            }
        }
    }
}

static void LIBUSB_CALL cb(struct libusb_transfer *xfr) {
    if (xfr->status != LIBUSB_TRANSFER_COMPLETED &&
        xfr->status != LIBUSB_TRANSFER_CANCELLED)
        xfr_errs++;
    for (int i = 0; i < xfr->num_iso_packets; i++) {
        if (xfr->iso_packet_desc[i].status != LIBUSB_TRANSFER_COMPLETED) out_err++;
        else out_pkts++;
    }
    if (running) {
        fill_tone(xfr->buffer, xfr->num_iso_packets);   /* next chunk of tone */
        if (libusb_submit_transfer(xfr) == 0) return;
    }
    inflight--;
}

int main(int argc, char **argv) {
    int seconds = 5;
    if (argc > 1) seconds = atoi(argv[1]);
    if (argc > 2) freq = atof(argv[2]);
    if (argc > 3) amp = pow(10.0, atof(argv[3]) / 20.0);
    if (seconds <= 0) seconds = 5;
    signal(SIGINT, on_sigint);

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx)) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "DJM-T1 not found / no access\n"); return 2; }
    libusb_set_auto_detach_kernel_driver(h, 1);

    int r;
    if ((r = libusb_claim_interface(h, IFACE)) < 0) { fprintf(stderr, "claim: %s\n", libusb_strerror(r)); return 3; }
    if ((r = libusb_set_interface_alt_setting(h, IFACE, ALT)) < 0) { fprintf(stderr, "alt: %s\n", libusb_strerror(r)); return 4; }

    struct libusb_transfer *xfrs[NXFR];
    for (int i = 0; i < NXFR; i++) {
        unsigned char *buf = calloc(NPKT, PKT);
        fill_tone(buf, NPKT);
        xfrs[i] = libusb_alloc_transfer(NPKT);
        libusb_fill_iso_transfer(xfrs[i], h, EP_OUT, buf, NPKT * PKT, NPKT, cb, NULL, 1000);
        libusb_set_iso_packet_lengths(xfrs[i], PKT);
        if (libusb_submit_transfer(xfrs[i]) == 0) inflight++;
    }
    printf("playing %.0f Hz tone (%.1f dBFS) on all %d ch, EP 0x%02x, for %ds...\n",
           freq, 20 * log10(amp), CH, EP_OUT, seconds);

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (running) {
        struct timeval tv = {0, 200000};
        libusb_handle_events_timeout(ctx, &tv);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - t0.tv_sec) >= seconds) running = 0;
    }
    running = 0;
    for (int i = 0; i < NXFR; i++) libusb_cancel_transfer(xfrs[i]);
    while (inflight > 0) { struct timeval tv = {0, 100000}; libusb_handle_events_timeout(ctx, &tv); }

    printf("\n=== results ===\n");
    printf("out packets: %llu   (~%.2f s)   packet errors: %llu   transfer errors: %llu\n",
           (unsigned long long)out_pkts, out_pkts / 1000.0,
           (unsigned long long)out_err, (unsigned long long)xfr_errs);

    libusb_set_interface_alt_setting(h, IFACE, 0);
    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
