// djmt1_iso - full-duplex isochronous streaming engine for the Pioneer DJM-T1.
// Opens the device, claims interface 0 / alt 1, and runs both iso endpoints on a
// dedicated libusb thread. Audio crosses to/from the caller as interleaved 6-channel
// 32-bit float via two ring buffers. Shared by the CLI tools and the PipeWire node.
// SPDX-License-Identifier: MIT
#ifndef DJMT1_ISO_H
#define DJMT1_ISO_H

#define DJMT1_CH 6
#define DJMT1_RATE 48000

typedef struct djmt1_iso djmt1_iso;

// Start streaming. ring_frames = capacity (per direction) in frames.
// Returns NULL on failure (device not found, no access, claim failed).
djmt1_iso *djmt1_iso_start(int ring_frames);
void djmt1_iso_stop(djmt1_iso *e);

// Capture: copy up to n frames (interleaved 6ch float) from the device into dst.
// Returns frames actually available/copied (0 if none yet).
int djmt1_iso_read(djmt1_iso *e, float *dst, int n);

// Playback: queue up to n frames (interleaved 6ch float) for the device.
// Returns frames actually accepted (may be < n if the ring is full).
int djmt1_iso_write(djmt1_iso *e, const float *src, int n);

// Diagnostics.
unsigned djmt1_iso_capture_overruns(djmt1_iso *e);   // device data dropped (consumer too slow)
unsigned djmt1_iso_playback_underruns(djmt1_iso *e); // silence sent (producer too slow)
unsigned djmt1_iso_xfer_errors(djmt1_iso *e);

#endif
