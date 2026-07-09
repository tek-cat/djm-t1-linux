// djmt1_iso - full-duplex isochronous engine. See djmt1_iso.h.
// SPDX-License-Identifier: MIT
#include "djmt1_iso.h"
#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define VID 0x08e4
#define PID 0x015e
#define IFACE 0
#define ALT 1
#define EP_IN 0x82
#define EP_OUT 0x01
#define PKT_IN_MAX 1024
#define PKT_OUT 864
#define FRAMES_PER_PKT (PKT_OUT / (DJMT1_CH * 3))    /* 48 */
#define IN_MAX_FRAMES  (PKT_IN_MAX / (DJMT1_CH * 3))  /* 56 (a packet can be up to 1024 B) */
#define NPKT 24
#define NXFR 6

/* ---- lock-free SPSC ring of interleaved 6ch float frames ---- */
typedef struct { float *buf; int cap; _Atomic int w, r; } ring;
static int ring_init(ring *R, int frames) {
    R->cap = frames + 1;               /* one spare slot */
    R->buf = calloc((size_t)R->cap * DJMT1_CH, sizeof(float));
    atomic_store(&R->w, 0); atomic_store(&R->r, 0);
    return R->buf ? 0 : -1;
}
static void ring_free(ring *R) { free(R->buf); R->buf = NULL; }
static int ring_avail(ring *R) { int w = atomic_load(&R->w), r = atomic_load(&R->r); return (w - r + R->cap) % R->cap; }
static int ring_space(ring *R) { return R->cap - 1 - ring_avail(R); }
static int ring_write(ring *R, const float *src, int n) {
    int sp = ring_space(R); if (n > sp) n = sp;
    int w = atomic_load(&R->w);
    for (int i = 0; i < n; i++)
        memcpy(R->buf + ((w + i) % R->cap) * DJMT1_CH, src + (size_t)i * DJMT1_CH, DJMT1_CH * sizeof(float));
    atomic_store(&R->w, (w + n) % R->cap);
    return n;
}
static int ring_read(ring *R, float *dst, int n) {
    int av = ring_avail(R); if (n > av) n = av;
    int r = atomic_load(&R->r);
    for (int i = 0; i < n; i++)
        memcpy(dst + (size_t)i * DJMT1_CH, R->buf + ((r + i) % R->cap) * DJMT1_CH, DJMT1_CH * sizeof(float));
    atomic_store(&R->r, (r + n) % R->cap);
    return n;
}

struct djmt1_iso {
    libusb_context *ctx; libusb_device_handle *h;
    pthread_t thread; atomic_int running;
    struct libusb_transfer *in_x[NXFR], *out_x[NXFR];
    atomic_int inflight;
    ring cap_ring, play_ring;
    atomic_uint cap_over, play_under, xfer_err;
};

static inline float s24_to_f(const unsigned char *p) {
    int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x800000) v |= (int32_t)0xFF000000;
    return (float)v / 8388608.0f;
}
static inline void f_to_s24(float x, unsigned char *p) {
    if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
    int32_t v = (int32_t)(x * 8388607.0f);
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF;
}

static void LIBUSB_CALL in_cb(struct libusb_transfer *x) {
    djmt1_iso *e = x->user_data;
    if (x->status != LIBUSB_TRANSFER_COMPLETED && x->status != LIBUSB_TRANSFER_CANCELLED)
        atomic_fetch_add(&e->xfer_err, 1);
    float tmp[IN_MAX_FRAMES * DJMT1_CH];
    for (int i = 0; i < x->num_iso_packets; i++) {
        struct libusb_iso_packet_descriptor *d = &x->iso_packet_desc[i];
        if (d->status != LIBUSB_TRANSFER_COMPLETED) continue;
        int nf = (int)d->actual_length / (DJMT1_CH * 3);
        if (nf <= 0) continue;
        if (nf > IN_MAX_FRAMES) nf = IN_MAX_FRAMES;
        const unsigned char *pkt = libusb_get_iso_packet_buffer_simple(x, i);
        for (int f = 0; f < nf; f++)
            for (int c = 0; c < DJMT1_CH; c++)
                tmp[f * DJMT1_CH + c] = s24_to_f(pkt + (f * DJMT1_CH + c) * 3);
        int wrote = ring_write(&e->cap_ring, tmp, nf);
        if (wrote < nf) atomic_fetch_add(&e->cap_over, 1);
    }
    if (atomic_load(&e->running) && libusb_submit_transfer(x) == 0) return;
    atomic_fetch_sub(&e->inflight, 1);
}

static void LIBUSB_CALL out_cb(struct libusb_transfer *x) {
    djmt1_iso *e = x->user_data;
    if (x->status != LIBUSB_TRANSFER_COMPLETED && x->status != LIBUSB_TRANSFER_CANCELLED)
        atomic_fetch_add(&e->xfer_err, 1);
    int total = x->num_iso_packets * FRAMES_PER_PKT;
    float tmp[NPKT * FRAMES_PER_PKT * DJMT1_CH];
    int got = ring_read(&e->play_ring, tmp, total);
    if (got < total) {
        atomic_fetch_add(&e->play_under, 1);
        memset(tmp + (size_t)got * DJMT1_CH, 0, (size_t)(total - got) * DJMT1_CH * sizeof(float));
    }
    unsigned char *b = x->buffer;   /* contiguous: 18 bytes/frame back to back */
    for (int f = 0; f < total; f++)
        for (int c = 0; c < DJMT1_CH; c++)
            f_to_s24(tmp[f * DJMT1_CH + c], b + (f * DJMT1_CH + c) * 3);
    if (atomic_load(&e->running) && libusb_submit_transfer(x) == 0) return;
    atomic_fetch_sub(&e->inflight, 1);
}

static void *iso_thread(void *arg) {
    djmt1_iso *e = arg;
    while (atomic_load(&e->running)) {
        struct timeval tv = {0, 100000};
        libusb_handle_events_timeout(e->ctx, &tv);
    }
    return NULL;
}

djmt1_iso *djmt1_iso_start(int ring_frames) {
    djmt1_iso *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    if (libusb_init(&e->ctx)) { free(e); return NULL; }
    e->h = libusb_open_device_with_vid_pid(e->ctx, VID, PID);
    if (!e->h) { libusb_exit(e->ctx); free(e); return NULL; }
    libusb_set_auto_detach_kernel_driver(e->h, 1);
    if (libusb_claim_interface(e->h, IFACE) < 0 ||
        libusb_set_interface_alt_setting(e->h, IFACE, ALT) < 0) {
        libusb_close(e->h); libusb_exit(e->ctx); free(e); return NULL;
    }
    ring_init(&e->cap_ring, ring_frames);
    ring_init(&e->play_ring, ring_frames);
    atomic_store(&e->running, 1);
    for (int i = 0; i < NXFR; i++) {
        e->in_x[i] = libusb_alloc_transfer(NPKT);
        libusb_fill_iso_transfer(e->in_x[i], e->h, EP_IN, malloc(NPKT * PKT_IN_MAX),
                                 NPKT * PKT_IN_MAX, NPKT, in_cb, e, 1000);
        libusb_set_iso_packet_lengths(e->in_x[i], PKT_IN_MAX);
        if (libusb_submit_transfer(e->in_x[i]) == 0) atomic_fetch_add(&e->inflight, 1);

        e->out_x[i] = libusb_alloc_transfer(NPKT);
        libusb_fill_iso_transfer(e->out_x[i], e->h, EP_OUT, calloc(NPKT, PKT_OUT),
                                 NPKT * PKT_OUT, NPKT, out_cb, e, 1000);
        libusb_set_iso_packet_lengths(e->out_x[i], PKT_OUT);
        if (libusb_submit_transfer(e->out_x[i]) == 0) atomic_fetch_add(&e->inflight, 1);
    }
    pthread_create(&e->thread, NULL, iso_thread, e);
    return e;
}

void djmt1_iso_stop(djmt1_iso *e) {
    if (!e) return;
    atomic_store(&e->running, 0);
    for (int i = 0; i < NXFR; i++) {
        if (e->in_x[i]) libusb_cancel_transfer(e->in_x[i]);
        if (e->out_x[i]) libusb_cancel_transfer(e->out_x[i]);
    }
    pthread_join(e->thread, NULL);
    for (int i = 0; i < NXFR; i++) {
        if (e->in_x[i]) { free(e->in_x[i]->buffer); libusb_free_transfer(e->in_x[i]); }
        if (e->out_x[i]) { free(e->out_x[i]->buffer); libusb_free_transfer(e->out_x[i]); }
    }
    libusb_set_interface_alt_setting(e->h, IFACE, 0);
    libusb_release_interface(e->h, IFACE);
    libusb_close(e->h);
    libusb_exit(e->ctx);
    ring_free(&e->cap_ring); ring_free(&e->play_ring);
    free(e);
}

int djmt1_iso_read(djmt1_iso *e, float *dst, int n) { return ring_read(&e->cap_ring, dst, n); }
int djmt1_iso_write(djmt1_iso *e, const float *src, int n) { return ring_write(&e->play_ring, src, n); }
unsigned djmt1_iso_capture_overruns(djmt1_iso *e) { return atomic_load(&e->cap_over); }
unsigned djmt1_iso_playback_underruns(djmt1_iso *e) { return atomic_load(&e->play_under); }
unsigned djmt1_iso_xfer_errors(djmt1_iso *e) { return atomic_load(&e->xfer_err); }
