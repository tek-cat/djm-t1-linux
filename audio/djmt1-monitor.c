// djmt1-monitor - exercises the djmt1_iso engine full-duplex: pulls capture and
// reports per-channel levels, and (optionally) feeds a tone to playback. Validates
// the engine (threading, rings, both directions) independently of PipeWire.
//
// Usage: ./djmt1-monitor [seconds] [--tone]
// SPDX-License-Identifier: MIT
#include "djmt1_iso.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

int main(int argc, char **argv) {
    int seconds = 5, tone = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tone")) tone = 1;
        else seconds = atoi(argv[i]);
    }
    if (seconds <= 0) seconds = 5;

    djmt1_iso *e = djmt1_iso_start(DJMT1_RATE / 5);   /* 200 ms rings */
    if (!e) { fprintf(stderr, "engine start failed (device/access?)\n"); return 1; }
    printf("engine up (6ch @ %d Hz), running %ds%s...\n", DJMT1_RATE, seconds, tone ? " with tone" : "");

    float peak[DJMT1_CH] = {0}, sumsq[DJMT1_CH] = {0};
    unsigned long long frames = 0;
    double phase = 0;
    float cap[256 * DJMT1_CH], play[256 * DJMT1_CH];

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int got = djmt1_iso_read(e, cap, 256);
        for (int f = 0; f < got; f++)
            for (int c = 0; c < DJMT1_CH; c++) {
                float x = cap[f * DJMT1_CH + c], a = x < 0 ? -x : x;
                if (a > peak[c]) peak[c] = a;
                sumsq[c] += (double)x * x;
            }
        frames += got;

        if (tone) {
            for (int f = 0; f < 256; f++) {
                float s = 0.06f * sinf((float)phase);
                phase += 2.0 * M_PI * 440.0 / DJMT1_RATE;
                if (phase > 2 * M_PI) phase -= 2 * M_PI;
                for (int c = 0; c < DJMT1_CH; c++) play[f * DJMT1_CH + c] = s;
            }
            djmt1_iso_write(e, play, 256);
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - t0.tv_sec) >= seconds) break;
        struct timespec s = {0, 3 * 1000000}; nanosleep(&s, NULL);   /* ~3 ms */
    }

    printf("\n=== engine results ===\n");
    printf("captured frames: %llu (~%.2f s)\n", frames, frames / (double)DJMT1_RATE);
    printf("xfer errors: %u   capture overruns: %u   playback underruns: %u\n",
           djmt1_iso_xfer_errors(e), djmt1_iso_capture_overruns(e), djmt1_iso_playback_underruns(e));
    printf("per-channel level (peak / rms, dBFS):\n");
    for (int c = 0; c < DJMT1_CH; c++) {
        double rms = frames ? sqrt(sumsq[c] / frames) : 0;
        printf("  ch%d : peak %7.1f  rms %7.1f\n", c,
               peak[c] > 0 ? 20 * log10(peak[c]) : -144.0,
               rms > 0 ? 20 * log10(rms) : -144.0);
    }
    djmt1_iso_stop(e);
    return 0;
}
