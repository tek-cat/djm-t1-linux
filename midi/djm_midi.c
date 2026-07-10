// djm_midi - native Linux MIDI for the Pioneer DJM-T1.
//
// The DJM-T1 does not transmit MIDI when the kernel's generic drivers bind it.
// Reproducing the Windows driver's vendor "arm" writes is necessary but NOT
// sufficient: on Linux the mixer stays silent even armed, with the ALSA MIDI
// input pipe open. Capturing the working Windows session (usbmon over KVM
// passthrough) showed the driver keeps FOUR pipes live at once and the mixer
// mirrors its control surface to two of them:
//
//   iface0/alt1  iso audio  ep 0x82 IN / 0x01 OUT   (+ SET_CUR sampling freq)
//   iface2       USB-MIDI   ep 0x85 IN  (bulk)      <- the MIDI we want
//   iface3       HID        ep 0x87 IN  (interrupt) <- control surface mirror
//
// The mixer only starts transmitting once these are all up. This tool claims
// them as a single libusb owner, replays the arm, and bridges the USB-MIDI
// stream from ep 0x85 into an ALSA sequencer port named "Pioneer DJM-T1", so
// Mixxx (or aseqdump, or anything reading ALSA seq) sees an ordinary MIDI
// controller. Run it, then connect the port in Mixxx.
//
//   Build:  make            (needs libusb-1.0 and alsa-lib)
//   Run:    ./djm_midi      (needs device access; see ../udev/)
//   Flags:  -v  print every MIDI message   --no-audio / --no-hid  (diagnostics)
//
// SPDX-License-Identifier: MIT
#include <libusb-1.0/libusb.h>
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define VID 0x08e4
#define PID 0x015e

#define IFACE_AUDIO 0
#define ALT_AUDIO   1
#define EP_A_IN  0x82
#define EP_A_OUT 0x01
#define PKT_A_IN  1024
#define PKT_A_OUT 864
#define A_NPKT 24
#define A_NXFR 4

#define IFACE_MIDI 2
#define EP_MIDI_IN 0x85
#define EP_MIDI_OUT 0x04
#define MIDI_MAXPKT 512

#define IFACE_HID 3
#define EP_HID_IN 0x87
#define HID_LEN 16

static volatile sig_atomic_t do_quit = 0;
static void on_sig(int s) { (void)s; do_quit = 1; }

static libusb_device_handle *h;
static int audio_on = 1, hid_on = 1, verbose = 0;
static int running = 1;

// ALSA sequencer (duplex: device MIDI -> seq via encoder; seq -> device via decoder).
static snd_seq_t *seq;
static int seq_port;
static snd_midi_event_t *encoder;   // raw MIDI bytes -> seq events (device -> Mixxx)
static snd_midi_event_t *decoder;   // seq events -> raw MIDI bytes (Mixxx -> device, e.g. LEDs)

// USB-MIDI: byte0 = (cable<<4)|CIN; this table maps CIN -> #MIDI bytes that follow.
static const int cin_len[16] = {0,0,2,3,3,1,2,3,3,3,3,3,2,2,3,1};
static unsigned long midi_msgs = 0;

// ---- arm (identical to djm_arm) ----
static int wr(unsigned wv, unsigned wi) { return libusb_control_transfer(h, 0x40, 0x03, wv, wi, NULL, 0, 1000); }
static void rd(unsigned wi) { unsigned char b[3] = {0}; libusb_control_transfer(h, 0xc0, 0x00, 0, wi, b, 3, 1000); }
static int arm(void) {
    int ok = 1;
    ok &= wr(0x1100, 0x8002) == 0; rd(0x8002);
    ok &= wr(0x2200, 0x8002) == 0; rd(0x8002);
    ok &= wr(0x030a, 0x8002) == 0; rd(0x8002);
    ok &= wr(0x0000, 0x8003) == 0;
    ok &= wr(0x0000, 0x8004) == 0;
    return ok;
}

// ---- device -> Mixxx: bridge raw MIDI bytes into the ALSA sequencer ----
static void emit_midi(const unsigned char *m, int n) {
    for (int i = 0; i < n; i++) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        if (snd_midi_event_encode_byte(encoder, m[i], &ev) == 1) {
            snd_seq_ev_set_source(&ev, seq_port);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            snd_seq_event_output_direct(seq, &ev);
        }
    }
}

// ---- Mixxx -> device: pack raw MIDI into USB-MIDI packets, write to ep 0x04
// (drives the mixer's LEDs / feedback). One 4-byte packet per channel-voice
// message; CIN is the status high-nibble, same table as parsing. ----
static void send_to_device(const unsigned char *midi, int len) {
    unsigned char pkt[64]; int pn = 0, i = 0;
    while (i < len && pn + 4 <= (int)sizeof(pkt)) {
        unsigned char status = midi[i];
        if (status < 0x80) { i++; continue; }      // skip stray data bytes
        unsigned char hi = status >> 4; int mlen; unsigned char cin;
        switch (hi) {
            case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: mlen = 3; cin = hi; break;
            case 0xC: case 0xD: mlen = 2; cin = hi; break;
            default: mlen = 1; cin = 0x0F; break;   // single-byte system/realtime
        }
        pkt[pn++] = cin;
        pkt[pn++] = status;
        pkt[pn++] = (mlen > 1 && i + 1 < len) ? midi[i + 1] : 0;
        pkt[pn++] = (mlen > 2 && i + 2 < len) ? midi[i + 2] : 0;
        i += mlen;
    }
    if (pn > 0) { int done = 0; libusb_bulk_transfer(h, EP_MIDI_OUT, pkt, pn, &done, 200); }
}

static void drain_seq_input(void) {
    snd_seq_event_t *ev;
    while (snd_seq_event_input_pending(seq, 1) > 0) {
        if (snd_seq_event_input(seq, &ev) < 0) break;
        unsigned char buf[32];
        long n = snd_midi_event_decode(decoder, buf, sizeof(buf), ev);
        if (n > 0) send_to_device(buf, (int)n);
    }
}

static void LIBUSB_CALL midi_cb(struct libusb_transfer *t) {
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i + 4 <= t->actual_length; i += 4) {
            unsigned char *p = t->buffer + i;
            if (!p[0] && !p[1] && !p[2] && !p[3]) continue;   // padding
            int n = cin_len[p[0] & 0x0f];
            if (n <= 0) continue;
            midi_msgs++;
            if (verbose) {
                printf("MIDI:");
                for (int j = 1; j <= n; j++) printf(" %02x", p[j]);
                printf("\n"); fflush(stdout);
            }
            emit_midi(p + 1, n);
        }
    } else if (t->status == LIBUSB_TRANSFER_NO_DEVICE) { do_quit = 1; return; }
    if (running && !do_quit) libusb_submit_transfer(t);
}
static void LIBUSB_CALL hid_cb(struct libusb_transfer *t) {
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) { do_quit = 1; return; }
    if (running && hid_on && !do_quit) libusb_submit_transfer(t);
}
static void LIBUSB_CALL audio_cb(struct libusb_transfer *t) {
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) { do_quit = 1; return; }
    if (running && audio_on && !do_quit) libusb_submit_transfer(t);
}

static int seq_setup(void) {
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return -1;
    snd_seq_set_client_name(seq, "Pioneer DJM-T1");
    // Duplex port: READ (device -> Mixxx) and WRITE (Mixxx -> device LEDs).
    seq_port = snd_seq_create_simple_port(seq, "DJM-T1 MIDI 1",
                   SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
                   SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                   SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
    if (seq_port < 0) return -1;
    if (snd_midi_event_new(64, &encoder) < 0 || snd_midi_event_new(64, &decoder) < 0) return -1;
    snd_midi_event_no_status(encoder, 1);   // always emit full status bytes
    snd_midi_event_no_status(decoder, 1);
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--no-audio")) audio_on = 0;
        else if (!strcmp(argv[i], "--no-hid")) hid_on = 0;
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (seq_setup() < 0) { fprintf(stderr, "djm_midi: ALSA sequencer setup failed\n"); return 1; }

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx)) { fprintf(stderr, "djm_midi: libusb_init failed\n"); return 1; }
    for (int tries = 0; tries < 20 && !h; tries++) {
        h = libusb_open_device_with_vid_pid(ctx, VID, PID);
        if (!h) { struct timeval tv = {0, 100000}; libusb_handle_events_timeout(ctx, &tv); }
    }
    if (!h) { fprintf(stderr, "djm_midi: DJM-T1 not found / no access (see ../udev/)\n"); return 2; }
    libusb_set_auto_detach_kernel_driver(h, 1);

    struct libusb_transfer *ain[A_NXFR] = {0}, *aout[A_NXFR] = {0};
    if (audio_on) {
        if (libusb_claim_interface(h, IFACE_AUDIO) < 0 ||
            libusb_set_interface_alt_setting(h, IFACE_AUDIO, ALT_AUDIO) < 0) {
            fprintf(stderr, "djm_midi: audio iface0/alt1 claim failed\n"); return 3;
        }
        unsigned char freq[3] = {0x80, 0xbb, 0x00};   // 48000 Hz, LE
        libusb_control_transfer(h, 0x22, 0x01, 0x0100, EP_A_IN, freq, 3, 1000);
        for (int i = 0; i < A_NXFR; i++) {
            ain[i] = libusb_alloc_transfer(A_NPKT);
            libusb_fill_iso_transfer(ain[i], h, EP_A_IN, malloc(A_NPKT * PKT_A_IN), A_NPKT * PKT_A_IN, A_NPKT, audio_cb, NULL, 1000);
            libusb_set_iso_packet_lengths(ain[i], PKT_A_IN);
            libusb_submit_transfer(ain[i]);
            aout[i] = libusb_alloc_transfer(A_NPKT);
            libusb_fill_iso_transfer(aout[i], h, EP_A_OUT, calloc(A_NPKT, PKT_A_OUT), A_NPKT * PKT_A_OUT, A_NPKT, audio_cb, NULL, 1000);
            libusb_set_iso_packet_lengths(aout[i], PKT_A_OUT);
            libusb_submit_transfer(aout[i]);
        }
    }

    if (libusb_claim_interface(h, IFACE_MIDI) < 0) { fprintf(stderr, "djm_midi: MIDI iface2 claim failed\n"); return 4; }
    struct libusb_transfer *mx = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(mx, h, EP_MIDI_IN, malloc(MIDI_MAXPKT), MIDI_MAXPKT, midi_cb, NULL, 0);
    libusb_submit_transfer(mx);

    struct libusb_transfer *hx = NULL;
    if (hid_on) {
        if (libusb_claim_interface(h, IFACE_HID) < 0) { fprintf(stderr, "djm_midi: warning, HID iface3 claim failed (MIDI may stay silent)\n"); hid_on = 0; }
        else {
            hx = libusb_alloc_transfer(0);
            libusb_fill_interrupt_transfer(hx, h, EP_HID_IN, malloc(HID_LEN), HID_LEN, hid_cb, NULL, 0);
            libusb_submit_transfer(hx);
        }
    }

    // Let the pipes settle, then arm (audio live + reads pending, as Windows does).
    for (int i = 0; i < 10 && !do_quit; i++) { struct timeval tv = {0, 50000}; libusb_handle_events_timeout(ctx, &tv); }
    if (!arm()) fprintf(stderr, "djm_midi: warning, an arm write errored\n");

    printf("djm_midi: DJM-T1 armed; ALSA seq port 'Pioneer DJM-T1:DJM-T1 MIDI 1' is live"
           " (audio=%s hid=%s). Connect it in Mixxx. Ctrl-C to stop.\n",
           audio_on ? "on" : "off", hid_on ? "on" : "off");
    fflush(stdout);

    while (!do_quit) {
        struct timeval tv = {0, 30000}; libusb_handle_events_timeout(ctx, &tv);
        drain_seq_input();   // forward any Mixxx -> device MIDI (LEDs)
    }

    // ---- teardown ----
    running = 0; audio_on = 0; hid_on = 0;
    libusb_cancel_transfer(mx);
    if (hx) libusb_cancel_transfer(hx);
    for (int i = 0; i < A_NXFR; i++) { if (ain[i]) libusb_cancel_transfer(ain[i]); if (aout[i]) libusb_cancel_transfer(aout[i]); }
    for (int i = 0; i < 5; i++) { struct timeval tv = {0, 100000}; libusb_handle_events_timeout(ctx, &tv); }
    libusb_release_interface(h, IFACE_MIDI);
    if (hx) libusb_release_interface(h, IFACE_HID);
    if (ain[0]) { libusb_set_interface_alt_setting(h, IFACE_AUDIO, 0); libusb_release_interface(h, IFACE_AUDIO); }
    libusb_close(h);
    libusb_exit(ctx);
    if (encoder) snd_midi_event_free(encoder);
    if (decoder) snd_midi_event_free(decoder);
    if (seq) snd_seq_close(seq);
    printf("\ndjm_midi: stopped (%lu MIDI messages bridged).\n", midi_msgs);
    return 0;
}
