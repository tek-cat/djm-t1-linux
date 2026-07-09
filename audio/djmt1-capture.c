// djmt1-capture - Phase 1a of the DJM-T1 audio driver.
// Claims interface 0 / alt 1 and streams the isochronous IN endpoint (0x82),
// de-interleaves 6 channels x 24-bit, and reports framing + per-channel levels.
// Proves the captured format (48 kHz, 24-bit, 6ch, 864-byte packets) end to end.
//
// Build: make (or: gcc -O2 -o djmt1-capture djmt1-capture.c -lusb-1.0 -lm)
// Run:   ./djmt1-capture [seconds] [-o raw.s24]   (needs audio-group access; see udev/)
//
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
#define EP_IN 0x82
#define PKT_MAX 1024
#define NPKT 32          /* iso packets per transfer (~32 ms) */
#define NXFR 8           /* ring depth */
#define CH 6
#define FRAME_BYTES (CH * 3)   /* 6 ch x 24-bit = 18 bytes/frame */

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

static uint64_t total_pkts = 0, total_bytes = 0, err_pkts = 0, xfr_errs = 0, frames = 0;
static uint64_t pktlen_hist[PKT_MAX + 1] = {0};
static double ch_peak[CH] = {0}, ch_sumsq[CH] = {0};
static FILE *rawf = NULL;
static int inflight = 0;

static inline int32_t s24le(const unsigned char *p) {
    int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x800000) v |= (int32_t)0xFF000000;   /* sign-extend 24 -> 32 */
    return v;
}

static void process_packet(const unsigned char *d, int len) {
    if (rawf) fwrite(d, 1, (size_t)len, rawf);
    int nf = len / FRAME_BYTES;
    for (int f = 0; f < nf; f++) {
        const unsigned char *fr = d + f * FRAME_BYTES;
        for (int c = 0; c < CH; c++) {
            double x = s24le(fr + c * 3) / 8388608.0;   /* normalize by 2^23 */
            double a = x < 0 ? -x : x;
            if (a > ch_peak[c]) ch_peak[c] = a;
            ch_sumsq[c] += x * x;
        }
        frames++;
    }
}

static void LIBUSB_CALL cb(struct libusb_transfer *xfr) {
    if (xfr->status != LIBUSB_TRANSFER_COMPLETED &&
        xfr->status != LIBUSB_TRANSFER_CANCELLED)
        xfr_errs++;
    for (int i = 0; i < xfr->num_iso_packets; i++) {
        struct libusb_iso_packet_descriptor *p = &xfr->iso_packet_desc[i];
        if (p->status != LIBUSB_TRANSFER_COMPLETED) { err_pkts++; continue; }
        int len = (int)p->actual_length;
        if (len == 0) continue;
        total_pkts++; total_bytes += (uint64_t)len;
        if (len <= PKT_MAX) pktlen_hist[len]++;
        process_packet(libusb_get_iso_packet_buffer_simple(xfr, i), len);
    }
    if (running && libusb_submit_transfer(xfr) == 0) return;
    inflight--;   /* not resubmitted */
}

int main(int argc, char **argv) {
    int seconds = 5;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { rawf = fopen(argv[++i], "wb"); }
        else seconds = atoi(argv[i]);
    }
    if (seconds <= 0) seconds = 5;
    signal(SIGINT, on_sigint);

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx)) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "DJM-T1 not found / no access (audio group + udev rule?)\n"); return 2; }
    libusb_set_auto_detach_kernel_driver(h, 1);   /* iface 0 has none, but be safe */

    int r;
    if ((r = libusb_claim_interface(h, IFACE)) < 0) {
        fprintf(stderr, "claim iface %d: %s\n", IFACE, libusb_strerror(r)); return 3;
    }
    if ((r = libusb_set_interface_alt_setting(h, IFACE, ALT)) < 0) {
        fprintf(stderr, "set alt %d: %s\n", ALT, libusb_strerror(r)); return 4;
    }

    struct libusb_transfer *xfrs[NXFR];
    for (int i = 0; i < NXFR; i++) {
        unsigned char *buf = malloc(NPKT * PKT_MAX);
        xfrs[i] = libusb_alloc_transfer(NPKT);
        libusb_fill_iso_transfer(xfrs[i], h, EP_IN, buf, NPKT * PKT_MAX, NPKT, cb, NULL, 1000);
        libusb_set_iso_packet_lengths(xfrs[i], PKT_MAX);
        if (libusb_submit_transfer(xfrs[i]) == 0) inflight++;
    }
    printf("streaming iface%d/alt%d EP 0x%02x for %ds (%d transfers x %d packets)...\n",
           IFACE, ALT, EP_IN, seconds, NXFR, NPKT);

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

    /* report */
    double secs = total_pkts / 1000.0;   /* 1 packet/ms */
    printf("\n=== results ===\n");
    printf("packets: %llu   bytes: %llu   frames: %llu   (~%.2f s of audio)\n",
           (unsigned long long)total_pkts, (unsigned long long)total_bytes,
           (unsigned long long)frames, secs);
    printf("packet errors: %llu   transfer errors: %llu\n",
           (unsigned long long)err_pkts, (unsigned long long)xfr_errs);
    printf("packet-size histogram (nonzero):\n");
    for (int L = 0; L <= PKT_MAX; L++)
        if (pktlen_hist[L]) printf("  %4d bytes : %llu packets\n", L, (unsigned long long)pktlen_hist[L]);
    printf("per-channel level (peak / RMS, dBFS):\n");
    for (int c = 0; c < CH; c++) {
        double rms = frames ? sqrt(ch_sumsq[c] / frames) : 0;
        double pk_db = ch_peak[c] > 0 ? 20 * log10(ch_peak[c]) : -144.0;
        double rms_db = rms > 0 ? 20 * log10(rms) : -144.0;
        printf("  ch%d : peak %7.1f  rms %7.1f\n", c, pk_db, rms_db);
    }
    if (rawf) { fclose(rawf); printf("raw S24LE interleaved samples written.\n"); }

    libusb_set_interface_alt_setting(h, IFACE, 0);
    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
