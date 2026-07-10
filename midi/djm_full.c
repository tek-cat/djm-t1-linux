// djm_full - unified Pioneer DJM-T1 daemon: the built-in soundcard (PipeWire) and
// the MIDI controller (ALSA sequencer) in ONE process. It owns all four USB pipes,
// so the MIDI gate's "a live audio session must exist" precondition is always met
// with no start-order coordination between services.
//
// It reuses the two proven pieces unchanged:
//   - audio: the djmt1_iso engine (../audio/djmt1_iso.c) on its own libusb context,
//     pumped to/from PipeWire exactly as djmt1-pipewire does;
//   - MIDI:  the djm_midi bridge logic on a SECOND libusb context in a thread
//     (claim iface 2 + 3, arm, poll HID ep 0x87 to un-gate, bridge ep 0x85 <-> ALSA
//     seq, drive LEDs via ep 0x04).
// Two libusb contexts to the same device (different interfaces) is exactly what the
// two-process setup already does; this just collapses them into one PID.
//
//   Build:  make djm_full   (needs libpipewire-0.3, libusb-1.0, alsa-lib)
//   Run:    ./djm_full       (Ctrl+C to stop)   Flags: -v
//
// NOTE: assembled from two hardware-verified programs but not yet run end-to-end on
// the mixer as a single process; the two-service setup remains the tested default.
// SPDX-License-Identifier: MIT
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <libusb-1.0/libusb.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include "../audio/djmt1_iso.h"

#define VID 0x08e4
#define PID 0x015e
#define IFACE_MIDI 2
#define EP_MIDI_IN 0x85
#define EP_MIDI_OUT 0x04
#define MIDI_MAXPKT 512
#define IFACE_HID 3
#define EP_HID_IN 0x87
#define HID_LEN 16

static volatile sig_atomic_t g_stop = 0;
static int verbose = 0;

/* ---------------- MIDI engine (second libusb context, own thread) ---------------- */
static libusb_device_handle *mh;         /* MIDI/HID handle */
static libusb_context *mctx;
static snd_seq_t *seq;
static int seq_port;
static snd_midi_event_t *encoder, *decoder;
static const int cin_len[16] = {0,0,2,3,3,1,2,3,3,3,3,3,2,2,3,1};
static unsigned long midi_msgs = 0;

static int wr(unsigned wv, unsigned wi) { return libusb_control_transfer(mh, 0x40, 0x03, wv, wi, NULL, 0, 1000); }
static void rd(unsigned wi) { unsigned char b[3] = {0}; libusb_control_transfer(mh, 0xc0, 0x00, 0, wi, b, 3, 1000); }
static int arm(void) {
    int ok = 1;
    ok &= wr(0x1100, 0x8002) == 0; rd(0x8002);
    ok &= wr(0x2200, 0x8002) == 0; rd(0x8002);
    ok &= wr(0x030a, 0x8002) == 0; rd(0x8002);
    ok &= wr(0x0000, 0x8003) == 0;
    ok &= wr(0x0000, 0x8004) == 0;
    return ok;
}

static void emit_midi(const unsigned char *m, int n) {
    for (int i = 0; i < n; i++) {
        snd_seq_event_t ev; snd_seq_ev_clear(&ev);
        if (snd_midi_event_encode_byte(encoder, m[i], &ev) == 1) {
            snd_seq_ev_set_source(&ev, seq_port);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            snd_seq_event_output_direct(seq, &ev);
        }
    }
}
static void send_to_device(const unsigned char *midi, int len) {
    unsigned char pkt[64]; int pn = 0, i = 0;
    while (i < len && pn + 4 <= (int)sizeof(pkt)) {
        unsigned char status = midi[i];
        if (status < 0x80) { i++; continue; }
        unsigned char hi = status >> 4; int mlen; unsigned char cin;
        switch (hi) {
            case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: mlen = 3; cin = hi; break;
            case 0xC: case 0xD: mlen = 2; cin = hi; break;
            default: mlen = 1; cin = 0x0F; break;
        }
        pkt[pn++] = cin; pkt[pn++] = status;
        pkt[pn++] = (mlen > 1 && i + 1 < len) ? midi[i + 1] : 0;
        pkt[pn++] = (mlen > 2 && i + 2 < len) ? midi[i + 2] : 0;
        i += mlen;
    }
    if (pn > 0) { int done = 0; libusb_bulk_transfer(mh, EP_MIDI_OUT, pkt, pn, &done, 200); }
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
            if (!p[0] && !p[1] && !p[2] && !p[3]) continue;
            int n = cin_len[p[0] & 0x0f];
            if (n <= 0) continue;
            midi_msgs++;
            if (verbose) { printf("MIDI:"); for (int j = 1; j <= n; j++) printf(" %02x", p[j]); printf("\n"); fflush(stdout); }
            emit_midi(p + 1, n);
        }
    } else if (t->status == LIBUSB_TRANSFER_NO_DEVICE) { g_stop = 1; return; }
    if (!g_stop) libusb_submit_transfer(t);
}
static void LIBUSB_CALL hid_cb(struct libusb_transfer *t) {
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) { g_stop = 1; return; }
    if (!g_stop) libusb_submit_transfer(t);
}

static int seq_setup(void) {
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return -1;
    snd_seq_set_client_name(seq, "Pioneer DJM-T1");
    seq_port = snd_seq_create_simple_port(seq, "DJM-T1 MIDI 1",
                   SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
                   SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                   SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
    if (seq_port < 0) return -1;
    if (snd_midi_event_new(64, &encoder) < 0 || snd_midi_event_new(64, &decoder) < 0) return -1;
    snd_midi_event_no_status(encoder, 1); snd_midi_event_no_status(decoder, 1);
    return 0;
}

// The MIDI thread: opens its own libusb context, claims iface 2+3, submits the
// bulk/interrupt reads, arms (audio is already live), then services events + seq
// input until g_stop.
static void *midi_thread(void *arg) {
    (void)arg;
    if (libusb_init(&mctx)) { fprintf(stderr, "djm_full: MIDI libusb_init failed\n"); return NULL; }
    mh = libusb_open_device_with_vid_pid(mctx, VID, PID);
    if (!mh) { fprintf(stderr, "djm_full: MIDI open failed\n"); libusb_exit(mctx); return NULL; }
    libusb_set_auto_detach_kernel_driver(mh, 1);
    if (libusb_claim_interface(mh, IFACE_MIDI) < 0) { fprintf(stderr, "djm_full: claim MIDI iface failed\n"); goto out; }

    struct libusb_transfer *mx = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(mx, mh, EP_MIDI_IN, malloc(MIDI_MAXPKT), MIDI_MAXPKT, midi_cb, NULL, 0);
    libusb_submit_transfer(mx);

    struct libusb_transfer *hx = NULL;
    if (libusb_claim_interface(mh, IFACE_HID) == 0) {
        hx = libusb_alloc_transfer(0);
        libusb_fill_interrupt_transfer(hx, mh, EP_HID_IN, malloc(HID_LEN), HID_LEN, hid_cb, NULL, 0);
        libusb_submit_transfer(hx);
    } else fprintf(stderr, "djm_full: warning, HID claim failed (MIDI may stay silent)\n");

    for (int i = 0; i < 10 && !g_stop; i++) { struct timeval tv = {0, 50000}; libusb_handle_events_timeout(mctx, &tv); }
    if (!arm()) fprintf(stderr, "djm_full: warning, an arm write errored\n");
    fprintf(stderr, "djm_full: MIDI armed; ALSA seq port 'Pioneer DJM-T1' live.\n");

    while (!g_stop) {
        struct timeval tv = {0, 30000};
        libusb_handle_events_timeout(mctx, &tv);
        drain_seq_input();
    }

    libusb_cancel_transfer(mx);
    if (hx) libusb_cancel_transfer(hx);
    for (int i = 0; i < 5; i++) { struct timeval tv = {0, 100000}; libusb_handle_events_timeout(mctx, &tv); }
    libusb_release_interface(mh, IFACE_MIDI);
    if (hx) libusb_release_interface(mh, IFACE_HID);
out:
    libusb_close(mh);
    libusb_exit(mctx);
    return NULL;
}

/* ---------------- Audio (PipeWire + djmt1_iso), from djmt1-pipewire.c ---------------- */
struct app {
    struct pw_main_loop *loop;
    struct pw_stream *source, *sink;
    djmt1_iso *engine;
};
#define FRAME (sizeof(float) * DJMT1_CH)

static void on_source_process(void *userdata) {
    struct app *a = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->source);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    float *dst = d->data;
    if (dst) {
        int maxframes = d->maxsize / FRAME;
        int want = b->requested ? (int)b->requested : maxframes;
        if (want > maxframes) want = maxframes;
        int got = djmt1_iso_read(a->engine, dst, want);
        if (got < want) memset(dst + (size_t)got * DJMT1_CH, 0, (size_t)(want - got) * FRAME);
        d->chunk->offset = 0; d->chunk->stride = FRAME; d->chunk->size = (uint32_t)(want * FRAME);
    }
    pw_stream_queue_buffer(a->source, b);
}
static void on_sink_process(void *userdata) {
    struct app *a = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->sink);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk->size) djmt1_iso_write(a->engine, (const float *)d->data, d->chunk->size / FRAME);
    pw_stream_queue_buffer(a->sink, b);
}
static const struct pw_stream_events source_events = { PW_VERSION_STREAM_EVENTS, .process = on_source_process };
static const struct pw_stream_events sink_events   = { PW_VERSION_STREAM_EVENTS, .process = on_sink_process };

static void on_signal(void *userdata, int sig) { (void)sig; struct app *a = userdata; g_stop = 1; pw_main_loop_quit(a->loop); }

static struct pw_stream *make_stream(struct app *a, const char *name, const char *media_class,
                                     const char *category, enum pw_direction dir,
                                     const struct pw_stream_events *events) {
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, category,
        PW_KEY_MEDIA_CLASS, media_class, PW_KEY_NODE_NAME, name,
        PW_KEY_NODE_DESCRIPTION, "Pioneer DJM-T1", NULL);
    struct pw_stream *s = pw_stream_new_simple(pw_main_loop_get_loop(a->loop), name, props, events, a);
    uint8_t buf[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_F32, .channels = DJMT1_CH, .rate = DJMT1_RATE);
    const struct spa_pod *params[1] = { spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info) };
    pw_stream_connect(s, dir, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1);
    return s;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = 1;
    pw_init(&argc, &argv);
    struct app a = {0};

    // 1) Audio first, so a live session exists before the MIDI thread arms (the gate).
    a.engine = djmt1_iso_start(DJMT1_RATE / 5);
    if (!a.engine) { fprintf(stderr, "djm_full: audio engine start failed (device/access?)\n"); return 1; }

    // 2) ALSA seq + MIDI thread (its own libusb context on iface 2+3).
    if (seq_setup() < 0) { fprintf(stderr, "djm_full: ALSA sequencer setup failed\n"); djmt1_iso_stop(a.engine); return 1; }
    pthread_t mt;
    if (pthread_create(&mt, NULL, midi_thread, NULL) != 0) { fprintf(stderr, "djm_full: MIDI thread failed\n"); djmt1_iso_stop(a.engine); return 1; }

    // 3) PipeWire main loop for the audio device.
    a.loop = pw_main_loop_new(NULL);
    pw_loop_add_signal(pw_main_loop_get_loop(a.loop), SIGINT, on_signal, &a);
    pw_loop_add_signal(pw_main_loop_get_loop(a.loop), SIGTERM, on_signal, &a);
    a.source = make_stream(&a, "DJM-T1 Capture", "Audio/Source", "Capture", PW_DIRECTION_OUTPUT, &source_events);
    a.sink   = make_stream(&a, "DJM-T1 Playback", "Audio/Sink", "Playback", PW_DIRECTION_INPUT, &sink_events);
    fprintf(stderr, "djm_full: DJM-T1 is a 6ch soundcard + MIDI controller. Ctrl+C to stop.\n");
    pw_main_loop_run(a.loop);

    // shutdown
    g_stop = 1;
    pthread_join(mt, NULL);
    pw_stream_destroy(a.source);
    pw_stream_destroy(a.sink);
    pw_main_loop_destroy(a.loop);
    djmt1_iso_stop(a.engine);
    if (encoder) snd_midi_event_free(encoder);
    if (decoder) snd_midi_event_free(decoder);
    if (seq) snd_seq_close(seq);
    pw_deinit();
    fprintf(stderr, "djm_full: stopped (%lu MIDI messages).\n", midi_msgs);
    return 0;
}
