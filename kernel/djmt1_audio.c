// SPDX-License-Identifier: (GPL-2.0 OR MIT)
//
// djmt1_audio - kernel ALSA driver for the Pioneer DJM-T1's built-in soundcard.
//
// The DJM-T1's audio interface (iface 0) is vendor-specific isochronous, not
// USB-Audio-Class, so the generic snd-usb-audio driver does not bind it. This is
// a standalone ALSA card that streams it directly. The wire format was
// reverse-engineered by the userspace tools in ../audio/ and is fixed:
//   iface 0, alt 1; EP 0x01 OUT / 0x82 IN; isochronous; 864-byte packets;
//   48 kHz, 24-bit, 6 channels; 18 bytes/frame (S24_3LE interleaved).
// Because the device's frame layout is exactly ALSA's S24_3LE 6-channel
// interleaved, the URB payload is copied to/from the PCM ring with no conversion.
//
// STATUS: assembled from the verified userspace format, but NOT yet loaded or
// tested on hardware. Review before insmod; loading an untested USB/ALSA module
// can wedge a device or oops. The userspace PipeWire driver in ../audio/ is the
// tested path. This is the upstream-track kernel tier.
//
// Build (out-of-tree): make -C kernel   (needs the running kernel's build tree)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/spinlock.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#define DRV "djmt1_audio"
#define VID 0x08e4
#define PID 0x015e
#define IFACE 0
#define ALT 1
#define EP_OUT 0x01
#define EP_IN  0x82
#define PKT_BYTES 864
#define FRAME_BYTES 18                          /* 6 ch * 3 bytes */
#define FRAMES_PER_PKT (PKT_BYTES / FRAME_BYTES) /* 48 */
#define RATE 48000
#define CHANNELS 6
#define NPKT 8                                  /* iso packets per URB */
#define NURB 4                                  /* URBs in flight per direction */
#define URB_BYTES (PKT_BYTES * NPKT)

struct djmt1_stream {
	struct snd_pcm_substream *substream;
	struct urb *urb[NURB];
	unsigned int hwptr;      /* frame offset into the ALSA ring */
	unsigned int period_pos; /* frames since last period elapse */
	atomic_t running;
};

struct djmt1 {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct djmt1_stream capture; /* device -> host, EP_IN  */
	struct djmt1_stream playback;/* host -> device, EP_OUT */
	spinlock_t lock;
};

static const struct snd_pcm_hardware djmt1_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = RATE, .rate_max = RATE,
	.channels_min = CHANNELS, .channels_max = CHANNELS,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = FRAME_BYTES * FRAMES_PER_PKT,
	.period_bytes_max = 64 * 1024,
	.periods_min = 2, .periods_max = 1024,
};

/*
 * Advance the hw pointer by nframes, copying to/from the ALSA ring; elapse a
 * period when we cross a period boundary. Caller holds no lock; runtime is safe
 * inside the URB completion. Returns via snd_pcm_period_elapsed when due.
 */
static void djmt1_transfer(struct djmt1_stream *s, unsigned char *pkt,
			   unsigned int nframes, bool capture)
{
	struct snd_pcm_substream *ss = s->substream;
	struct snd_pcm_runtime *rt = ss->runtime;
	unsigned int buf_frames = rt->buffer_size;
	unsigned int i, off;

	for (i = 0; i < nframes; i++) {
		off = (s->hwptr + i) % buf_frames;
		if (capture)
			memcpy(rt->dma_area + off * FRAME_BYTES,
			       pkt + i * FRAME_BYTES, FRAME_BYTES);
		else
			memcpy(pkt + i * FRAME_BYTES,
			       rt->dma_area + off * FRAME_BYTES, FRAME_BYTES);
	}
	s->hwptr = (s->hwptr + nframes) % buf_frames;
	s->period_pos += nframes;
	if (s->period_pos >= rt->period_size) {
		s->period_pos %= rt->period_size;
		snd_pcm_period_elapsed(ss);
	}
}

static void djmt1_in_complete(struct urb *urb)
{
	struct djmt1_stream *s = urb->context;
	int i;

	if (!atomic_read(&s->running) || urb->status)
		goto resubmit;
	for (i = 0; i < urb->number_of_packets; i++) {
		unsigned char *pkt = urb->transfer_buffer + urb->iso_frame_desc[i].offset;
		unsigned int len = urb->iso_frame_desc[i].actual_length;

		if (urb->iso_frame_desc[i].status || len < FRAME_BYTES)
			continue;
		djmt1_transfer(s, pkt, len / FRAME_BYTES, true);
	}
resubmit:
	if (atomic_read(&s->running)) {
		for (i = 0; i < urb->number_of_packets; i++) {
			urb->iso_frame_desc[i].offset = i * PKT_BYTES;
			urb->iso_frame_desc[i].length = PKT_BYTES;
		}
		usb_submit_urb(urb, GFP_ATOMIC);
	}
}

static void djmt1_out_complete(struct urb *urb)
{
	struct djmt1_stream *s = urb->context;
	int i;

	if (!atomic_read(&s->running))
		return;
	for (i = 0; i < urb->number_of_packets; i++) {
		unsigned char *pkt = urb->transfer_buffer + i * PKT_BYTES;

		urb->iso_frame_desc[i].offset = i * PKT_BYTES;
		urb->iso_frame_desc[i].length = PKT_BYTES;
		djmt1_transfer(s, pkt, FRAMES_PER_PKT, false);
	}
	usb_submit_urb(urb, GFP_ATOMIC);
}

static int djmt1_alloc_urbs(struct djmt1 *chip, struct djmt1_stream *s,
			    unsigned int ep, usb_complete_t complete)
{
	int u, p;

	for (u = 0; u < NURB; u++) {
		struct urb *urb = usb_alloc_urb(NPKT, GFP_KERNEL);

		if (!urb)
			return -ENOMEM;
		s->urb[u] = urb;
		urb->dev = chip->udev;
		urb->pipe = (ep & USB_DIR_IN) ?
			usb_rcvisocpipe(chip->udev, ep & 0x0f) :
			usb_sndisocpipe(chip->udev, ep & 0x0f);
		urb->interval = 1;
		urb->transfer_flags = URB_ISO_ASAP;
		urb->number_of_packets = NPKT;
		urb->transfer_buffer_length = URB_BYTES;
		urb->context = s;
		urb->complete = complete;
		urb->transfer_buffer = kzalloc(URB_BYTES, GFP_KERNEL);
		if (!urb->transfer_buffer)
			return -ENOMEM;
		for (p = 0; p < NPKT; p++) {
			urb->iso_frame_desc[p].offset = p * PKT_BYTES;
			urb->iso_frame_desc[p].length = PKT_BYTES;
		}
	}
	return 0;
}

static void djmt1_free_urbs(struct djmt1_stream *s)
{
	int u;

	for (u = 0; u < NURB; u++) {
		if (s->urb[u]) {
			usb_kill_urb(s->urb[u]);
			kfree(s->urb[u]->transfer_buffer);
			usb_free_urb(s->urb[u]);
			s->urb[u] = NULL;
		}
	}
}

static int djmt1_open(struct snd_pcm_substream *ss)
{
	ss->runtime->hw = djmt1_hw;
	return 0;
}

static int djmt1_close(struct snd_pcm_substream *ss)
{
	return 0;
}

static int djmt1_prepare(struct snd_pcm_substream *ss)
{
	struct djmt1 *chip = snd_pcm_substream_chip(ss);
	struct djmt1_stream *s = (ss->stream == SNDRV_PCM_STREAM_CAPTURE) ?
		&chip->capture : &chip->playback;

	s->hwptr = 0;
	s->period_pos = 0;
	/* Activate the iso interface once (harmless if already alt 1). */
	usb_set_interface(chip->udev, IFACE, ALT);
	return 0;
}

static int djmt1_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct djmt1 *chip = snd_pcm_substream_chip(ss);
	struct djmt1_stream *s = (ss->stream == SNDRV_PCM_STREAM_CAPTURE) ?
		&chip->capture : &chip->playback;
	int u, ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		atomic_set(&s->running, 1);
		for (u = 0; u < NURB; u++) {
			ret = usb_submit_urb(s->urb[u], GFP_ATOMIC);
			if (ret) {
				atomic_set(&s->running, 0);
				return ret;
			}
		}
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		atomic_set(&s->running, 0);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t djmt1_pointer(struct snd_pcm_substream *ss)
{
	struct djmt1 *chip = snd_pcm_substream_chip(ss);
	struct djmt1_stream *s = (ss->stream == SNDRV_PCM_STREAM_CAPTURE) ?
		&chip->capture : &chip->playback;

	return s->hwptr;
}

static const struct snd_pcm_ops djmt1_ops = {
	.open = djmt1_open,
	.close = djmt1_close,
	.prepare = djmt1_prepare,
	.trigger = djmt1_trigger,
	.pointer = djmt1_pointer,
};

static int djmt1_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct snd_card *card;
	struct djmt1 *chip;
	int err;

	/* Only bind the vendor iso audio interface (iface 0). */
	if (intf->cur_altsetting->desc.bInterfaceNumber != IFACE)
		return -ENODEV;

	err = snd_card_new(&intf->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			   THIS_MODULE, sizeof(*chip), &card);
	if (err < 0)
		return err;
	chip = card->private_data;
	chip->udev = udev;
	chip->intf = intf;
	chip->card = card;
	spin_lock_init(&chip->lock);

	strscpy(card->driver, DRV, sizeof(card->driver));
	strscpy(card->shortname, "Pioneer DJM-T1", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "Pioneer DJM-T1 at %s", dev_name(&udev->dev));

	err = snd_pcm_new(card, "DJM-T1", 0, 1, 1, &chip->pcm);
	if (err < 0)
		goto fail;
	chip->pcm->private_data = chip;
	strscpy(chip->pcm->name, "Pioneer DJM-T1", sizeof(chip->pcm->name));
	snd_pcm_set_ops(chip->pcm, SNDRV_PCM_STREAM_PLAYBACK, &djmt1_ops);
	snd_pcm_set_ops(chip->pcm, SNDRV_PCM_STREAM_CAPTURE, &djmt1_ops);
	snd_pcm_set_managed_buffer_all(chip->pcm, SNDRV_DMA_TYPE_VMALLOC,
				       NULL, 0, 0);

	chip->capture.substream = chip->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
	chip->playback.substream = chip->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;

	err = usb_set_interface(udev, IFACE, ALT);
	if (err < 0)
		goto fail;
	err = djmt1_alloc_urbs(chip, &chip->capture, EP_IN, djmt1_in_complete);
	if (err < 0)
		goto fail_urbs;
	err = djmt1_alloc_urbs(chip, &chip->playback, EP_OUT, djmt1_out_complete);
	if (err < 0)
		goto fail_urbs;

	err = snd_card_register(card);
	if (err < 0)
		goto fail_urbs;

	usb_set_intfdata(intf, chip);
	dev_info(&intf->dev, "%s: Pioneer DJM-T1 ALSA card registered\n", DRV);
	return 0;

fail_urbs:
	djmt1_free_urbs(&chip->capture);
	djmt1_free_urbs(&chip->playback);
fail:
	snd_card_free(card);
	return err;
}

static void djmt1_disconnect(struct usb_interface *intf)
{
	struct djmt1 *chip = usb_get_intfdata(intf);

	if (!chip)
		return;
	atomic_set(&chip->capture.running, 0);
	atomic_set(&chip->playback.running, 0);
	djmt1_free_urbs(&chip->capture);
	djmt1_free_urbs(&chip->playback);
	snd_card_free_when_closed(chip->card);
	usb_set_intfdata(intf, NULL);
}

static const struct usb_device_id djmt1_ids[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(VID, PID, IFACE) },
	{ }
};
MODULE_DEVICE_TABLE(usb, djmt1_ids);

static struct usb_driver djmt1_driver = {
	.name = DRV,
	.probe = djmt1_probe,
	.disconnect = djmt1_disconnect,
	.id_table = djmt1_ids,
};
module_usb_driver(djmt1_driver);

MODULE_AUTHOR("Aaron Landis");
MODULE_DESCRIPTION("Pioneer DJM-T1 built-in soundcard (vendor iso audio) ALSA driver");
MODULE_LICENSE("Dual MIT/GPL");
