/*
 * GAPK (GSM Audio Packet Knife) based audio I/O
 *
 * (C) 2017 by Vadim Yanitskiy <axilirator@gmail.com>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <string.h>
#include <errno.h>

#include <osmocom/core/msgb.h>

#include <osmocom/gapk/procqueue.h>
#include <osmocom/gapk/formats.h>
#include <osmocom/gapk/codecs.h>
#include <osmocom/gapk/common.h>

#include <osmocom/bb/common/osmocom_data.h>
#include <osmocom/bb/common/logging.h>

#include <osmocom/bb/mobile/mncc.h>
#include <osmocom/bb/mobile/voice.h>

/* The RAW PCM format is common for both audio source and sink */
static const struct osmo_gapk_format_desc *rawpcm_fmt;

static int pq_queue_tch_fb_recv(void *_state, uint8_t *out,
	const uint8_t *in, unsigned int in_len)
{
	struct gapk_io_state *state = (struct gapk_io_state *) _state;
	struct msgb *tch_msg;
	size_t frame_len;

	/* Obtain one TCH frame from the DL buffer */
	tch_msg = msgb_dequeue(&state->tch_fb_dl);

	/* Make sure we've got a frame */
	if (!tch_msg)
		return -EIO;

	/* FIXME: determine frame_len depending on a codec used */
	frame_len = FR_CANON_LEN;

	/* Copy the frame bytes from message */
	memcpy(out, tch_msg->l2h, frame_len);

	/* Release memory */
	msgb_free(tch_msg);

	return frame_len;
}

static int pq_queue_tch_fb_send(void *_state, uint8_t *out,
	const uint8_t *in, unsigned int in_len)
{
	struct gapk_io_state *state = (struct gapk_io_state *) _state;
	struct msgb *tch_msg;

	/* Allocate a new message for the lower layers */
	tch_msg = msgb_alloc_headroom(FR_CANON_LEN + 64, 64, "TCH frame");
	if (!tch_msg)
		return -ENOMEM;

	/* Copy the frame bytes to a new message */
	tch_msg->l2h = msgb_put(tch_msg, FR_CANON_LEN);
	memcpy(tch_msg->l2h, in, in_len);

	/* Put encoded TCH frame to the UL buffer */
	msgb_enqueue(&state->tch_fb_ul, tch_msg);

	return 0;
}

/**
 * A custom TCH frame buffer block, which actually
 * handles incoming frames from DL buffer and puts
 * outgoing frames to UL buffer...
 */
static int pq_queue_tch_fb(struct osmo_gapk_pq *pq,
	struct gapk_io_state *io_state, int is_src)
{
	struct osmo_gapk_pq_item *item;

	LOGP(DGAPK, LOGL_DEBUG, "PQ '%s': Adding TCH frame buffer %s\n",
		pq->name, is_src ? "input" : "output");

	/* Allocate and add a new queue item */
	item = osmo_gapk_pq_add_item(pq);
	if (!item)
		return -ENOMEM;

	/* General item type and description */
	item->type = is_src ?
		OSMO_GAPK_ITEM_TYPE_SOURCE : OSMO_GAPK_ITEM_TYPE_SINK;
	item->cat_name = is_src ? "source" : "sink";
	item->sub_name = "tch_io";

	/* I/O length */
	item->len_in  = is_src ? 0 : FR_CANON_LEN;
	item->len_out = is_src ? FR_CANON_LEN : 0;

	/* Handler and it's state */
	item->proc = is_src ?
		pq_queue_tch_fb_recv : pq_queue_tch_fb_send;
	item->state = io_state;

	return 0;
}

/**
 * Prepares the following queue (source is mic):
 * src/alsa -> proc/codec -> sink/tch_fb
 */
static int prepare_audio_source(struct osmocom_ms *ms,
	enum osmo_gapk_codec_type codec)
{
	const struct osmo_gapk_codec_desc *codec_out;
	struct gsm_settings *set = &ms->settings;
	struct osmo_gapk_pq *pq;
	char *pq_desc;
	int rc;

	LOGP(DGAPK, LOGL_DEBUG, "Prepare audio input chain "
		"for MS '%s'\n", ms->name);

	/* Determine the output codec */
	codec_out = osmo_gapk_codec_get_from_type(codec);
	if (!codec_out)
		return -ENOTSUP;

	/* Allocate a processing queue */
	pq = osmo_gapk_pq_create("pq_audio_source");
	if (!pq)
		return -ENOMEM;

	/* ALSA audio source */
	rc = osmo_gapk_pq_queue_alsa_input(pq,
		set->audio.alsa_input_dev, rawpcm_fmt->frame_len);
	if (rc)
		goto error;

	/* Frame encoder */
	rc = osmo_gapk_pq_queue_codec(pq, codec_out, 1);
	if (rc)
		goto error;

	/* TCH frame buffer sink */
	rc = pq_queue_tch_fb(pq, ms->gapk_io, 0);
	if (rc)
		goto error;

	/* Check composed queue in strict mode */
	rc = osmo_gapk_pq_check(pq, 1);
	if (rc)
		goto error;

	/* Prepare queue (allocate buffers, etc.) */
	rc = osmo_gapk_pq_prepare(pq);
	if (rc)
		goto error;

	/* Save pointer within MS GAPK state */
	ms->gapk_io->pq_source = pq;

	/* Describe prepared chain */
	pq_desc = osmo_gapk_pq_describe(pq);
	LOGP(DGAPK, LOGL_DEBUG, "PQ '%s': chain '%s' prepared\n",
		pq->name, pq_desc);
	talloc_free(pq_desc);

	return 0;

error:
	talloc_free(pq);
	return rc;
}

/**
 * Prepares the following queue (sink is speaker):
 * src/tch_fb -> proc/codec -> sink/alsa
 */
static int prepare_audio_sink(struct osmocom_ms *ms,
	enum osmo_gapk_codec_type codec)
{
	const struct osmo_gapk_codec_desc *codec_in;
	struct gsm_settings *set = &ms->settings;
	struct osmo_gapk_pq *pq;
	char *pq_desc;
	int rc;

	LOGP(DGAPK, LOGL_DEBUG, "Prepare audio output chain "
		"for MS '%s'\n", ms->name);

	/* Determine the input codec */
	codec_in = osmo_gapk_codec_get_from_type(codec);
	if (!codec_in)
		return -ENOTSUP;

	/* Allocate a processing queue */
	pq = osmo_gapk_pq_create("pq_audio_sink");
	if (!pq)
		return -ENOMEM;

	/* TCH frame buffer source */
	rc = pq_queue_tch_fb(pq, ms->gapk_io, 1);
	if (rc)
		goto error;

	/* Frame decoder */
	rc = osmo_gapk_pq_queue_codec(pq, codec_in, 0);
	if (rc)
		goto error;

	/* ALSA audio sink */
	rc = osmo_gapk_pq_queue_alsa_output(pq,
		set->audio.alsa_output_dev, rawpcm_fmt->frame_len);
	if (rc)
		goto error;

	/* Check composed queue in strict mode */
	rc = osmo_gapk_pq_check(pq, 1);
	if (rc)
		goto error;

	/* Prepare queue (allocate buffers, etc.) */
	rc = osmo_gapk_pq_prepare(pq);
	if (rc)
		goto error;

	/* Save pointer within MS GAPK state */
	ms->gapk_io->pq_sink = pq;

	/* Describe prepared chain */
	pq_desc = osmo_gapk_pq_describe(pq);
	LOGP(DGAPK, LOGL_DEBUG, "PQ '%s': chain '%s' prepared\n",
		pq->name, pq_desc);
	talloc_free(pq_desc);

	return 0;

error:
	talloc_free(pq);
	return rc;
}

/**
 * Cleans up both TCH frame I/O buffers, destroys both
 * processing queues (chains), and deallocates the memory.
 * Should be called when a voice call is finished...
 */
int gapk_io_clean_up_ms(struct osmocom_ms *ms)
{
	struct msgb *msg;

	if (!ms->gapk_io)
		return 0;

	/* Flush TCH frame I/O buffers */
	while ((msg = msgb_dequeue(&ms->gapk_io->tch_fb_dl)))
		msgb_free(msg);
	while ((msg = msgb_dequeue(&ms->gapk_io->tch_fb_ul)))
		msgb_free(msg);

	/* Destroy both audio I/O chains */
	if (ms->gapk_io->pq_source)
		osmo_gapk_pq_destroy(ms->gapk_io->pq_source);
	if (ms->gapk_io->pq_sink)
		osmo_gapk_pq_destroy(ms->gapk_io->pq_sink);

	talloc_free(ms->gapk_io);

	return 0;
}

/**
 * Allocates both TCH frame I/O buffers
 * and prepares both processing queues (chains).
 * Should be called when a voice call is initiated...
 */
int gapk_io_init_ms(struct osmocom_ms *ms,
	enum osmo_gapk_codec_type codec)
{
	struct gapk_io_state *gapk_io;
	int rc = 0;

	/* Attempt to allocate memory */
	gapk_io = talloc_zero(ms, struct gapk_io_state);
	if (!gapk_io) {
		LOGP(DGAPK, LOGL_ERROR, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	/* Init TCH frame I/O buffers */
	INIT_LLIST_HEAD(&gapk_io->tch_fb_dl);
	INIT_LLIST_HEAD(&gapk_io->tch_fb_ul);

	/* Init pointers */
	ms->gapk_io = gapk_io;

	/* Prepare both source and sink chains */
	rc |= prepare_audio_source(ms, codec);
	rc |= prepare_audio_sink(ms, codec);

	if (rc)
		return rc;

	LOGP(DGAPK, LOGL_NOTICE, "GAPK I/O initialized for MS "
		"'%s'\n", ms->name);

	return 0;
}

/**
 * Performs basic initialization of GAPK library,
 * setting the talloc root context and a logging category.
 * Should be called during the application initialization...
 */
void gapk_io_init(void *talloc_ctx)
{
	/* Set talloc context */
	osmo_gapk_set_talloc_ctx(talloc_ctx);

	/* Init logging subsystem */
	osmo_gapk_log_init(DGAPK);

	/* Make RAWPCM format info easy to access */
	rawpcm_fmt = osmo_gapk_fmt_get_from_type(FMT_RAWPCM_S16LE);

	LOGP(DGAPK, LOGL_NOTICE, "init GAPK audio I/O\n");
}

/* Serves both TCH frame I/O buffers */
int gapk_io_dequeue(struct osmocom_ms *ms)
{
	struct gapk_io_state *gapk_io = ms->gapk_io;
	struct llist_head *entry;
	size_t frame_count = 0;
	int work = 0;

	/* There is no active call, nothing to do */
	if (!gapk_io)
		return 0;

	/**
	 * Make sure we have at least two frames
	 * to prevent discontinuous playback.
	 */
	llist_for_each(entry, &gapk_io->tch_fb_dl)
		if (++frame_count > 2)
			break;
	if (frame_count < 2)
		return 0;

	/**
	 * TODO: if there is an active call, but no TCH frames
	 * in DL buffer, put silence frames using the upcoming
	 * ECU (Error Concealment Unit) of libosmocodec.
	 */
	while (!llist_empty(&gapk_io->tch_fb_dl)) {
		DEBUGP(DGAPK, "Processing DL TCH frame...\n");

		/* Decode and play received DL TCH frame */
		osmo_gapk_pq_execute(gapk_io->pq_sink);

		/* Record and encode an UL TCH frame back */
		osmo_gapk_pq_execute(gapk_io->pq_source);

		work |= 1;
	}

	while (!llist_empty(&gapk_io->tch_fb_ul)) {
		struct gsm_data_frame *frame;
		struct msgb *tch_msg;

		DEBUGP(DGAPK, "Processing UL TCH frame...\n");

		/* Obtain one TCH frame from the UL buffer */
		tch_msg = msgb_dequeue(&gapk_io->tch_fb_ul);

		/* Prepend frame header */
		frame = (struct gsm_data_frame *)
			msgb_push(tch_msg, sizeof(struct gsm_data_frame));

		/* FIXME: prepare frame header */
		frame->callref = ms->mncc_entity.ref;
		frame->msg_type = GSM_TCHF_FRAME;

		/* Push a voice frame to the lower layers */
		gsm_send_voice(ms, frame);

		/* Release memory */
		talloc_free(tch_msg);

		work |= 1;
	}

	return work;
}
