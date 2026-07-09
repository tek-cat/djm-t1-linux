// djmt1-pipewire - Phase 2: present the DJM-T1 as a real PipeWire audio device.
// Creates a 6-channel source (capture) and a 6-channel sink (playback), both backed
// by the shared djmt1_iso engine. Run it and the mixer shows up in wpctl/pavucontrol.
//
// Build: make djmt1-pipewire    Run: ./djmt1-pipewire   (Ctrl+C to stop)
// SPDX-License-Identifier: MIT
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <signal.h>
#include <string.h>
#include "djmt1_iso.h"

struct app {
    struct pw_main_loop *loop;
    struct pw_stream *source;   /* capture: device -> graph */
    struct pw_stream *sink;     /* playback: graph -> device */
    struct spa_source *timer;
    djmt1_iso *engine;
};

static void on_timer(void *userdata, uint64_t expirations) {
    (void)expirations;
    struct app *a = userdata;
    fprintf(stderr, "[djmt1] xfer-errors=%u capture-overruns=%u playback-underruns=%u\n",
            djmt1_iso_xfer_errors(a->engine), djmt1_iso_capture_overruns(a->engine),
            djmt1_iso_playback_underruns(a->engine));
}

#define FRAME (sizeof(float) * DJMT1_CH)   /* 24 bytes/frame, F32 x 6 */

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
        d->chunk->offset = 0;
        d->chunk->stride = FRAME;
        d->chunk->size = (uint32_t)(want * FRAME);
    }
    pw_stream_queue_buffer(a->source, b);
}

static void on_sink_process(void *userdata) {
    struct app *a = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->sink);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk->size) {
        int nframes = d->chunk->size / FRAME;
        djmt1_iso_write(a->engine, (const float *)d->data, nframes);
    }
    pw_stream_queue_buffer(a->sink, b);
}

static const struct pw_stream_events source_events = {
    PW_VERSION_STREAM_EVENTS, .process = on_source_process,
};
static const struct pw_stream_events sink_events = {
    PW_VERSION_STREAM_EVENTS, .process = on_sink_process,
};

static void do_quit(void *userdata, int sig) {
    (void)sig; struct app *a = userdata; pw_main_loop_quit(a->loop);
}

static struct pw_stream *make_stream(struct app *a, const char *name, const char *media_class,
                                     const char *category, enum pw_direction dir,
                                     const struct pw_stream_events *events) {
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, category,
        PW_KEY_MEDIA_CLASS, media_class,
        PW_KEY_NODE_NAME, name,
        PW_KEY_NODE_DESCRIPTION, "Pioneer DJM-T1",
        NULL);
    struct pw_stream *s = pw_stream_new_simple(pw_main_loop_get_loop(a->loop), name, props, events, a);

    uint8_t buf[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_F32, .channels = DJMT1_CH, .rate = DJMT1_RATE);
    const struct spa_pod *params[1] = { spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info) };

    pw_stream_connect(s, dir, PW_ID_ANY,
        PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1);
    return s;
}

int main(int argc, char **argv) {
    pw_init(&argc, &argv);
    struct app a = {0};

    a.engine = djmt1_iso_start(DJMT1_RATE / 5);   /* 200 ms rings */
    if (!a.engine) { fprintf(stderr, "djmt1-pipewire: engine start failed (device/access?)\n"); return 1; }

    a.loop = pw_main_loop_new(NULL);
    pw_loop_add_signal(pw_main_loop_get_loop(a.loop), SIGINT, do_quit, &a);
    pw_loop_add_signal(pw_main_loop_get_loop(a.loop), SIGTERM, do_quit, &a);

    a.timer = pw_loop_add_timer(pw_main_loop_get_loop(a.loop), on_timer, &a);
    struct timespec value = { 5, 0 }, interval = { 5, 0 };   /* health line every 5 s */
    pw_loop_update_timer(pw_main_loop_get_loop(a.loop), a.timer, &value, &interval, false);

    a.source = make_stream(&a, "DJM-T1 Capture", "Audio/Source", "Capture",
                           PW_DIRECTION_OUTPUT, &source_events);
    a.sink   = make_stream(&a, "DJM-T1 Playback", "Audio/Sink", "Playback",
                           PW_DIRECTION_INPUT, &sink_events);

    fprintf(stderr, "djmt1-pipewire: DJM-T1 is now a 6ch source + 6ch sink. Ctrl+C to stop.\n");
    pw_main_loop_run(a.loop);

    pw_stream_destroy(a.source);
    pw_stream_destroy(a.sink);
    pw_main_loop_destroy(a.loop);
    djmt1_iso_stop(a.engine);
    pw_deinit();
    return 0;
}
