/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <inttypes.h>
#include <alsa/asoundlib.h>

#define LOG_MODULE "audio"

#include "log.h"
#include "debug.h"
#include "su.h"
#include "linkedlist.h"
#include "compiler.h"
#include "time_util.h"
#include "math_util.h"
#include "queue.h"
#include "audio.h"


#define AVBOX_AUDIOSTREAM_DATA_PACKET	(1)
#define AVBOX_AUDIOSTREAM_CLOCK_SET	(2)


struct avbox_audiostream_data_packet
{
	size_t n_frames;
	uint8_t *data;
};


struct avbox_audiostream_clock_set
{
	int64_t value;
};


/**
 * Structure for storing audio packets.
 */
LISTABLE_STRUCT(avbox_audio_packet,
	int type;
	union {
		struct avbox_audiostream_data_packet data_packet;
		struct avbox_audiostream_clock_set clock_set;
	};
);


/**
 * Audio stream structure.
 */
struct avbox_audiostream
{
	snd_pcm_t *pcm_handle;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	pthread_t thread;
	int quit;
	int paused;
	int running;
	int started;
	int max_frames;
	int queued_frames;
	int blocking;
	int64_t frames;
	int64_t clock_start;
	int64_t clock_offset;
	snd_pcm_uframes_t buffer_size;
	unsigned int framerate;
	struct avbox_queue *packets;
	avbox_audiostream_callback callback;
	void *callback_context;
};


/**
 * Calculate the amount of time (in useconds) that it would
 * take to play a given amount of frames
 */
#define FRAMES2TIME(stream, frames)	(((frames) * 1000.0 * 1000.0) / (double) stream->framerate)


/**
 * Get the size in bytes of a number of audio samples.
 */
static inline size_t
avbox_audiostream_frames2size(struct avbox_audiostream * const stream,
	snd_pcm_uframes_t frames)
{
	assert(stream != NULL);

	/* For now this is hardcoded to 4 bytes per frame since we
	 * only support 16-bit stereo */
	(void) stream;
	return frames * 4;
}


static int
avbox_audiostream_pcm_drain(struct avbox_audiostream * const inst)
{
	int err;
	snd_pcm_sframes_t frames;

	/* NOTE: snd_pcm_drain() is racy and may deadlock (I think
	 * when the ringbuffer underruns before it is called) so you
	 * cannot be 100% sure that it won't happen, even if you check
	 * that the buffer is full before calling it. So instead we'll
	 * just wait for the stream to underrun */
	while ((frames = snd_pcm_avail(inst->pcm_handle)) > 0 && frames < inst->buffer_size) {
		usleep(FRAMES2TIME(inst, (inst->buffer_size - frames)));
	}

	/* reset ALSA. The clock will start running again when the
	 * audio starts playing (after we write more packets) */
	if ((err = snd_pcm_reset(inst->pcm_handle)) < 0) {
		LOG_VPRINT_ERROR("Could not reset PCM: %s",
			snd_strerror(err));
		return -1;
	}
	if ((err = snd_pcm_prepare(inst->pcm_handle)) < 0) {
		return -1;
	}
	return 0;
}


/**
 * Flush the queue
 */
static void
__avbox_audiostream_drop(struct avbox_audiostream * const stream)
{
	DEBUG_PRINT("audio", "Dropping queue");
	struct avbox_audio_packet *packet;
	while (avbox_queue_count(stream->packets) > 0) {
		packet = avbox_queue_get(stream->packets);
		if (packet->type == AVBOX_AUDIOSTREAM_DATA_PACKET) {
			ASSERT(packet != NULL);
			stream->queued_frames -= packet->data_packet.n_frames;
		}
		free(packet);
	}
	ASSERT(stream->queued_frames == 0);

}


/**
 * Flush an audio stream.
 */
void
avbox_audiostream_drop(struct avbox_audiostream * const inst)
{
	pthread_mutex_lock(&inst->lock);
	avbox_audiostream_pcm_drain(inst);

	__avbox_audiostream_drop(inst);
	pthread_cond_broadcast(&inst->wake);
	pthread_mutex_unlock(&inst->lock);
}


/**
 * Gets a string for a pcm state.
 */
#ifndef NDEBUG
static const char *
avbox_pcm_state_getstring(snd_pcm_state_t state)
{
	switch (state) {
	case SND_PCM_STATE_OPEN: return "OPEN";
	case SND_PCM_STATE_SETUP: return "SETUP";
	case SND_PCM_STATE_PREPARED: return "PREPARED";
	case SND_PCM_STATE_RUNNING: return "RUNNING";
	case SND_PCM_STATE_XRUN: return "XRUN";
	case SND_PCM_STATE_DRAINING: return "DRAINING";
	case SND_PCM_STATE_PAUSED: return "PAUSED";
	case SND_PCM_STATE_SUSPENDED: return "SUSPENDED";
	case SND_PCM_STATE_DISCONNECTED: return "DISCONNECTED";
	default: return "UNKNOWN";
	}
}
#endif


/**
 * Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
int64_t
__avbox_audiostream_gettime(struct avbox_audiostream * const stream)
{
	int err = 0;
	snd_pcm_status_t *status;

	if (stream->pcm_handle == NULL) {
		goto end;
	}

	/* if the stream is pause or hasn't started return
	 * the internal offset */
	if (stream->paused || stream->frames == 0) {
		goto end;
	}

	/* make sure we're not in xrun */
	if ((err = snd_pcm_avail(stream->pcm_handle)) < 0) {
		/* LOG_VPRINT_ERROR("avbox_audiostream_gettime(): ALSA error detected: %s",
			snd_strerror(err)); */
		goto end;
	}

	/* get the status */
	snd_pcm_status_alloca(&status);
	if ((err = snd_pcm_status(stream->pcm_handle, status)) < 0) {
		LOG_VPRINT_ERROR("Stream status error: %s", snd_strerror(err));
		goto end;
	}

	/* if the stream is running calculate it's runtime
	 * based on the internal offset + timestamp - trigger timestamp */
	switch (snd_pcm_status_get_state(status)) {
	case SND_PCM_STATE_XRUN:
	case SND_PCM_STATE_OPEN:
	case SND_PCM_STATE_SETUP:
	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_PAUSED:
	case SND_PCM_STATE_SUSPENDED:
	case SND_PCM_STATE_DISCONNECTED:
		break;
	case SND_PCM_STATE_RUNNING:
	case SND_PCM_STATE_DRAINING:
	{
		int64_t time;
		snd_timestamp_t ts, tts;
		snd_pcm_status_get_trigger_tstamp(status, &tts);
		snd_pcm_status_get_tstamp(status, &ts);
		time = stream->clock_start + stream->clock_offset;
		time += SEC2USEC(ts.tv_sec) + ts.tv_usec;
		time -= SEC2USEC(tts.tv_sec) + tts.tv_usec;
		return time;
	}
	default:
		DEBUG_VPRINT("audio", "Unknown ALSA state (state=%i)",
			snd_pcm_status_get_state(status));
		abort();
	}
end:
	return stream->clock_start +
		(stream->framerate ? FRAMES2TIME(stream, stream->frames) : 0);
}


int64_t
avbox_audiostream_gettime(struct avbox_audiostream * const stream)
{
	int64_t ret;
	pthread_mutex_lock(&stream->lock);
	ret = __avbox_audiostream_gettime(stream);
	pthread_mutex_unlock(&stream->lock);
	return ret;
}


/**
 * Pauses the audio stream and synchronizes
 * the audio clock.
 */
int
avbox_audiostream_pause(struct avbox_audiostream * const inst)
{
	int err, ret = -1;
	snd_pcm_status_t *status;

	DEBUG_PRINT("audio", "Pausing audio stream");
	ASSERT(inst != NULL);

	if (!inst->pcm_handle) {
		inst->paused = 1;
		return 0;
	}

	snd_pcm_status_alloca(&status);

	pthread_mutex_lock(&inst->lock);

	/* we need to call this or snd_pcm_status() may succeed when
	 * we're on XRUN */
	if ((err = snd_pcm_avail(inst->pcm_handle)) < 0) {
		LOG_VPRINT_ERROR("Could not get pcm avail: %s",
			snd_strerror(err));
		inst->paused = 1;
		goto end;
	}

	/* get pcm handle status */
	if ((err = snd_pcm_status(inst->pcm_handle, status)) < 0) {
		LOG_VPRINT_ERROR("Stream status error: %s\n", snd_strerror(err));
		goto end;
	}

	switch (snd_pcm_status_get_state(status)) {
	case SND_PCM_STATE_OPEN:
	case SND_PCM_STATE_SETUP:
	case SND_PCM_STATE_PREPARED:
		inst->paused = 1;
		ret = 0;
		goto end;
	case SND_PCM_STATE_SUSPENDED:
	case SND_PCM_STATE_PAUSED:
		DEBUG_PRINT("audio", "Unexpected ALSA state");
		abort();
		break;
	case SND_PCM_STATE_XRUN:
	{
#ifndef NDEBUG
		const int64_t xruntime = inst->clock_start +
			FRAMES2TIME(inst, inst->frames);
		DEBUG_VPRINT("audio", "Pausing on XRUN: offset=%li xruntime=%li",
			inst->clock_offset, xruntime);
#endif
		inst->paused = 1;
		ret = 0;
		goto end;
	}
	case SND_PCM_STATE_RUNNING:
	{
		DEBUG_VPRINT("audio", "Pausing RUNNING stream (offset=%li)",
			inst->clock_offset);

		/* start draining the buffer */
		inst->paused = 1;
		avbox_audiostream_pcm_drain(inst);

		inst->clock_offset = FRAMES2TIME(inst, inst->frames);

		DEBUG_VPRINT(LOG_MODULE, "Predicted: %i, Actual: %li",
			inst->clock_start + FRAMES2TIME(inst, inst->frames),
			__avbox_audiostream_gettime(inst));
		DEBUG_VPRINT("audio", "PCM state after pause: %s",
			avbox_pcm_state_getstring(snd_pcm_state(inst->pcm_handle)));

		ret = 0;
		goto end;
	}
	case SND_PCM_STATE_DISCONNECTED:
	case SND_PCM_STATE_DRAINING:
		LOG_PRINT_ERROR("Invalid ALSA state");
		/* since we only drain in this function and we wait
		 * we should never hit this */
		abort();
		break;
	}

end:
	pthread_mutex_unlock(&inst->lock);
	return ret;
}


/**
 * Resume audio playback
 */
int
avbox_audiostream_resume(struct avbox_audiostream * const inst)
{
	int ret = -1;

	DEBUG_VPRINT("audio", "Resuming audio stream (time=%li)",
		avbox_audiostream_gettime(inst));

	pthread_mutex_lock(&inst->lock);

	if (!inst->paused) {
		LOG_PRINT_ERROR("Cannot resume non-paused stream");
		goto end;
	}

	inst->paused = 0;
	ret = 0;
end:
	/* signal IO thread */
	pthread_cond_broadcast(&inst->wake);
	pthread_mutex_unlock(&inst->lock);

	DEBUG_VPRINT("audio", "Audio stream resumed (time=%li)",
		avbox_audiostream_gettime(inst));

	return ret;
}


/**
 * This is the main playback loop.
 */
static void*
avbox_audiostream_output(void *arg)
{
	int ret, underrun = 1;
	int64_t timeout;
	size_t n_frames;
	struct avbox_audiostream * const inst = (struct avbox_audiostream * const) arg;
	struct avbox_audio_packet * packet;
	const char *device;
	unsigned int period_usecs = 10;
	int dir = 0;
	snd_pcm_hw_params_t *params;
	snd_pcm_sw_params_t *swparams;
	snd_pcm_sframes_t frames;
	snd_pcm_uframes_t period = 1024, silence_len,
		start_thres, stop_thres, silen_thres;
	const snd_pcm_uframes_t fragment = 1024;

	DEBUG_SET_THREAD_NAME("audio_output");
	DEBUG_PRINT(LOG_MODULE, "Audio playback thread started");

	ASSERT(inst != NULL);
	ASSERT(inst->pcm_handle == NULL);
	ASSERT(inst->quit == 0);
	ASSERT(inst->paused == 0);

	/* set the thread priority to realtime */
#ifdef ENABLE_REALTIME
	struct sched_param parms;
	parms.sched_priority = sched_get_priority_max(SCHED_RR) - 21;
	if (pthread_setschedparam(pthread_self(), SCHED_RR, &parms) != 0) {
		LOG_PRINT_ERROR("Could not send main thread priority");
	}
#endif

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_sw_params_alloca(&swparams);

	inst->framerate = 48000;

	(void) avbox_gainroot();

	/* if ALSA_DEVICE is set on the environment use that
	 * instead of the default device */
	if ((device = getenv("ALSA_DEVICE")) == NULL) {
		device = "default";
	}

	/* initialize alsa device */
	if ((ret = snd_pcm_open(&inst->pcm_handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		LOG_VPRINT_ERROR("snd_pcm_open() failed: %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_hw_params_any(inst->pcm_handle, params)) < 0) {
		LOG_VPRINT_ERROR("Broken ALSA configuration: none available. %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_hw_params_set_access(inst->pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		LOG_VPRINT_ERROR("INTERLEAVED RW access not available. %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_hw_params_set_format(inst->pcm_handle, params, SND_PCM_FORMAT_S16_LE)) < 0) {
		LOG_VPRINT_ERROR("Format S16_LE not supported. %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_hw_params_set_channels(inst->pcm_handle, params, 2)) < 0) {
		LOG_VPRINT_ERROR("2 Channels not available. %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_hw_params_set_rate_near(inst->pcm_handle, params, &inst->framerate, &dir)) < 0) {
		LOG_VPRINT_ERROR("48000Hz not available. %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_hw_params_set_period_size_near(inst->pcm_handle, params, &period, &dir)) < 0) {
		LOG_VPRINT_ERROR("Cannot set period. %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_hw_params(inst->pcm_handle, params)) < 0) {
		LOG_VPRINT_ERROR("Could not set ALSA params: %s", snd_strerror(ret));
		goto end;
	}

	if ((ret = snd_pcm_sw_params_current(inst->pcm_handle, swparams)) < 0) {
		LOG_VPRINT_ERROR("Could not determine SW params. %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_sw_params_set_tstamp_type(inst->pcm_handle, swparams, SND_PCM_TSTAMP_TYPE_MONOTONIC)) < 0) {
		LOG_VPRINT_ERROR("Could not set ALSA clock to CLOCK_MONOTONIC. %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_sw_params_set_tstamp_mode(inst->pcm_handle, swparams, SND_PCM_TSTAMP_ENABLE)) < 0) {
		LOG_VPRINT_ERROR("Could not enable timestamps: %s", snd_strerror(ret));
	}
	if ((ret = snd_pcm_sw_params_set_avail_min(inst->pcm_handle, swparams, fragment)) < 0) {
		LOG_VPRINT_ERROR("Could not set ALSA avail_min: %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_sw_params(inst->pcm_handle, swparams)) < 0) {
		LOG_VPRINT_ERROR("Could not set ALSA SW paramms. %s", snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_pcm_hw_params_get_period_time(params, &period_usecs, &dir)) < 0) {
		LOG_VPRINT_ERROR("Could not get period time: %s",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_hw_params_get_rate(params, &inst->framerate, &dir)) < 0) {
		LOG_VPRINT_ERROR("Could not get framerate: %s",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_hw_params_get_period_size(params, &period, &dir)) < 0) {
		LOG_VPRINT_ERROR("Could not get period size: %s",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_hw_params_get_buffer_size(params, &inst->buffer_size)) < 0) {
		LOG_VPRINT_ERROR("Could not get buffer size: %s",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_sw_params_get_start_threshold(swparams, &start_thres)) < 0) {
		LOG_VPRINT_ERROR("Could not get start threshold: %s", snd_strerror(ret));
	}
	if ((ret = snd_pcm_sw_params_get_stop_threshold(swparams, &stop_thres)) < 0) {
		LOG_VPRINT_ERROR("Could not get stop threshold: %s", snd_strerror(ret));
	}
	if ((ret = snd_pcm_sw_params_get_silence_threshold(swparams, &silen_thres)) < 0) {
		LOG_VPRINT_ERROR("Could not get silence threshold: %s", snd_strerror(ret));
	}
	if ((ret = snd_pcm_sw_params_get_silence_size(swparams, &silence_len)) < 0) {
		LOG_VPRINT_ERROR("Could not get silence size: %s", snd_strerror(ret));
	}

	/* print debug info */
	DEBUG_VPRINT("audio", "ALSA library version: %s", SND_LIB_VERSION_STR);
	DEBUG_VPRINT("audio", "ALSA buffer size: %ld frames", (unsigned long) inst->buffer_size);
	DEBUG_VPRINT("audio", "ALSA period size: %ld frames", (unsigned long) period);
	DEBUG_VPRINT("audio", "ALSA period time: %ld usecs", period_usecs);
	DEBUG_VPRINT("audio", "ALSA framerate: %u Hz", inst->framerate);
	DEBUG_VPRINT("audio", "ALSA frame size: %lu bytes",
		avbox_audiostream_frames2size(inst, 1));
	DEBUG_VPRINT("audio", "ALSA free buffer space: %ld frames", snd_pcm_avail(inst->pcm_handle));
	DEBUG_VPRINT("audio", "ALSA Start threshold: %lu", start_thres);
	DEBUG_VPRINT("audio", "ALSA Stop threshold: %lu", stop_thres);
	DEBUG_VPRINT("audio", "ALSA Silence threshold: %lu", silen_thres);
	DEBUG_VPRINT("audio", "ALSA Silence size: %lu", silence_len);
	DEBUG_VPRINT("audio", "ALSA status: %s",
		avbox_pcm_state_getstring(snd_pcm_state(inst->pcm_handle)));
	DEBUG_VPRINT("audio", "Stream clock: %lu", inst->clock_start);

	snd_pcm_sframes_t delay = 0;
	snd_pcm_delay(inst->pcm_handle, &delay);
	DEBUG_VPRINT(LOG_MODULE, "delay: %"PRIi64,
		(int64_t) delay);

	/* drop superuser privileges */
	(void) avbox_droproot();

	/* signal that we've started successfully */
	inst->running = 1;
	pthread_mutex_lock(&inst->lock);
	pthread_cond_broadcast(&inst->wake);
	pthread_mutex_unlock(&inst->lock);

	DEBUG_PRINT("audio", "Audio thread ready");

	/* start audio IO */
	while (1) {
		/* get the next packet */
		if (!underrun) {

			/* calculate how long until the stream dries out
			 * and wait up to that long for new packets */
			pthread_mutex_lock(&inst->lock);
			if ((frames = snd_pcm_avail(inst->pcm_handle)) < 0) {
				DEBUG_PRINT(LOG_MODULE, "snd_pcm_avail() returned error!!!");
				timeout = 1;
			} else {
				timeout = FRAMES2TIME(inst, inst->buffer_size - frames);
			}
			pthread_mutex_unlock(&inst->lock);

			if ((packet = avbox_queue_timedpeek(inst->packets, timeout)) == NULL) {
				if (errno == EAGAIN) {
					snd_pcm_state_t state = snd_pcm_state(inst->pcm_handle);

					if (state == SND_PCM_STATE_RUNNING ||
						state == SND_PCM_STATE_PAUSED ||
						state == SND_PCM_STATE_SUSPENDED) {
						DEBUG_VPRINT(LOG_MODULE, "PCM state after timedpeek: %s. "
							"Still waiting (timeout=%"PRIi64" frames=%"PRIi64")",
							avbox_pcm_state_getstring(state), timeout, frames);
						continue;
					}

					underrun = 1;

					if (inst->callback != NULL) {
						inst->callback(inst, AVBOX_AUDIOSTREAM_UNDERRUN, inst->callback_context);
					}
					continue;
				} else if (errno == ESHUTDOWN) {
					goto end;
				} else {
					LOG_VPRINT_ERROR("Could not timedpeek packet from queue: %s",
						strerror(errno));
					goto end;
				}
			}
		} else {
			if ((packet = avbox_queue_peek(inst->packets, 1)) == NULL) {
				switch (errno) {
				case EAGAIN: continue;
				case ESHUTDOWN: goto end;
				default:
					LOG_VPRINT_ERROR("Could not peek packet from queue: %s",
						strerror(errno));
					goto end;
				}
			}
			underrun = 0;
		}

		/* if this is a control packet handle it */
		if (packet->type != AVBOX_AUDIOSTREAM_DATA_PACKET) {
			switch (packet->type) {
			case AVBOX_AUDIOSTREAM_CLOCK_SET:
			{
				snd_pcm_status_t *status;
				snd_pcm_status_alloca(&status);

				pthread_mutex_lock(&inst->lock);
				avbox_audiostream_pcm_drain(inst);

				DEBUG_VPRINT(LOG_MODULE, "Resetting clock to %d (was %li)",
					packet->clock_set.value, __avbox_audiostream_gettime(inst));

				/* reset the clock */
				inst->clock_start = packet->clock_set.value;
				inst->clock_offset = 0;
				inst->frames = ret = 0;
				pthread_mutex_unlock(&inst->lock);
				break;
			}
			default:
				ABORT("Invalid packet type!");
			}

			/* free the packet and continue */
			if (avbox_queue_get(inst->packets) != packet) {
				LOG_PRINT_ERROR("Peeked one packet but got a different one. Aborting");
				abort();
			}
			free(packet);
			continue;
		}

		pthread_mutex_lock(&inst->lock);

		/* wait for the pcm to be ready to receive a fragment.
		 * NOTE: We don't want to use snd_pcm_wait() for this as it
		 * deadlocks with some drivers */
		if ((ret = snd_pcm_avail(inst->pcm_handle)) > 0 && ret < fragment) {
			struct timespec tv;
			tv.tv_sec = 0;
			tv.tv_nsec = FRAMES2TIME(inst, fragment - ret) * 1000LL;
			delay2abstime(&tv);
			pthread_cond_timedwait(&inst->wake, &inst->lock, &tv);
			pthread_mutex_unlock(&inst->lock);
			continue;
		}

		/* check if we're paused */
		if (UNLIKELY(inst->quit)) {
			pthread_mutex_unlock(&inst->lock);
			goto end;
		} else if (UNLIKELY(inst->paused)) {
			pthread_cond_wait(&inst->wake, &inst->lock);
			pthread_mutex_unlock(&inst->lock);
			continue;
		} else if (UNLIKELY(avbox_queue_peek(inst->packets, 0) != packet)) {
			/* the packet changed. Mostlikely because the stream was
			 * flushed while we waited for the PCM */
			pthread_mutex_unlock(&inst->lock);
			continue;
		}

		/* calculate the number of frames to use from this packet */
		n_frames = MIN(fragment, packet->data_packet.n_frames);

		/* write fragment to ring buffer */
		if (UNLIKELY((frames = snd_pcm_writei(inst->pcm_handle, packet->data_packet.data, n_frames)) < 0)) {
			DEBUG_VPRINT("audio", "Recovering from ALSA error: %s",
				snd_strerror(frames));

			/* update the offset */
			inst->clock_offset = FRAMES2TIME(inst, inst->frames);

			/* attempt to recover */
			if ((frames = snd_pcm_recover(inst->pcm_handle, frames, 1)) < 0) {
				/* recovery has failed... Instead of bailing out we'll pretend
				 * that the write worked and invoke the critical error callback so
				 * that the controller thread can kill us. Otherwise things may deadlock
				 * since the pipeline is stalled */
				LOG_VPRINT_ERROR("Could not recover from ALSA error: %s",
					snd_strerror(frames));
				if (inst->callback != NULL) {
					inst->callback(inst, AVBOX_AUDIOSTREAM_CRITICAL_ERROR,
						inst->callback_context);
				}
				frames = n_frames;
			} else {
				ASSERT(frames == 0);
				pthread_mutex_unlock(&inst->lock);
				continue;
			}
		}

		/* update frame counts */
		inst->queued_frames -= frames;
		inst->frames += frames;
		packet->data_packet.data += avbox_audiostream_frames2size(inst, frames);
		packet->data_packet.n_frames -= frames;

		/* if there's no frames left in the packet then
		 * remove it from the queue and free it */
		if (packet->data_packet.n_frames == 0) {
			if (avbox_queue_get(inst->packets) != packet) {
				abort();
			}
			free(packet);
		}
		pthread_cond_broadcast(&inst->wake);
		pthread_mutex_unlock(&inst->lock);
	}

end:
	DEBUG_PRINT("audio", "Audio thread exiting");

	pthread_mutex_lock(&inst->lock);

	/* cleanup */
	if (inst->pcm_handle != NULL) {
		snd_pcm_hw_free(inst->pcm_handle);
		snd_pcm_close(inst->pcm_handle);
		inst->pcm_handle = NULL;
	}

	/* signal that we're quitting */
	inst->running = 0;
	pthread_cond_broadcast(&inst->wake);
	pthread_mutex_unlock(&inst->lock);

	return NULL;
}


/**
 * Sends a clock packet to the audio stream.
 */
int
avbox_audiostream_setclock(struct avbox_audiostream * const inst,
	const int64_t time)
{
	struct avbox_audio_packet *packet;

	if ((packet = malloc(sizeof(struct avbox_audio_packet))) == NULL) {
		ASSERT(errno == ENOMEM);
		return -1;
	}

	packet->type = AVBOX_AUDIOSTREAM_CLOCK_SET;
	packet->clock_set.value = time;

	/* add packet to queue */
	if (avbox_queue_put(inst->packets, packet) == -1) {
		LOG_VPRINT_ERROR("Could not add packet to queue: %s",
			strerror(errno));
		free(packet);
		return -1;
	}

	return 0;
}


/**
 * Check if the stream is blocking another thread on write().
 */
int
avbox_audiostream_blocking(struct avbox_audiostream * const stream)
{
	return stream->blocking;
}


/**
 * Writes n_frames audio frames to the stream.
 */
int
avbox_audiostream_write(struct avbox_audiostream * const stream,
	const uint8_t * const data, const size_t n_frames)
{
	struct avbox_audio_packet *packet;
	const int sz = avbox_audiostream_frames2size(stream, n_frames);

	ASSERT(stream != NULL);

	if (n_frames == 0) {
		return 0;
	}

	pthread_mutex_lock(&stream->lock);

	/* if the queue is full wait once and return EAGAIN
	 * if it's still full */
	if ((stream->queued_frames + n_frames) > stream->max_frames) {
		stream->blocking = 1;
		pthread_cond_wait(&stream->wake, &stream->lock);
		stream->blocking = 0;
		if ((stream->queued_frames + n_frames) > stream->max_frames) {
			pthread_mutex_unlock(&stream->lock);
			errno = EAGAIN;
			return -1;
		}
	}

	pthread_mutex_unlock(&stream->lock);

	/* allocate memory for packet */
	if ((packet = malloc(sizeof(struct avbox_audio_packet) + sz)) == NULL) {
		LOG_PRINT_ERROR("Could not allocate packet");
		errno = ENOMEM;
		return -1;
	}

	/* copy samples */
	packet->type = AVBOX_AUDIOSTREAM_DATA_PACKET;
	packet->data_packet.n_frames = n_frames;
	packet->data_packet.data = (uint8_t*) (packet + 1);
	memcpy(packet->data_packet.data, data, sz);

	pthread_mutex_lock(&stream->lock);

	/* add packet to queue */
	if (avbox_queue_put(stream->packets, packet) == -1) {
		LOG_VPRINT_ERROR("Could not add packet to queue: %s",
			strerror(errno));
		free(packet);
		return -1;
	}

	stream->queued_frames += n_frames;

	pthread_cond_broadcast(&stream->wake);
	pthread_mutex_unlock(&stream->lock);

	return 0;
}


/**
 * Starts the stream playback.
 */
int
avbox_audiostream_start(struct avbox_audiostream * const stream)
{
	int ret = -1;

	DEBUG_PRINT(LOG_MODULE, "Starting audio stream");

	pthread_mutex_lock(&stream->lock);

	if (stream->started) {
		LOG_PRINT_ERROR("Audio stream already started");
		errno = EEXIST;
		goto end;
	}

	if (pthread_create(&stream->thread, NULL, avbox_audiostream_output, stream) != 0) {
		LOG_PRINT_ERROR("Could not start IO thread");
		abort();
	}

	pthread_cond_wait(&stream->wake, &stream->lock);

	stream->started = 1;

	if (!stream->running) {
		LOG_PRINT_ERROR("Audio thread initialization failed");
		goto end;
	}

	ret = 0;
end:
	pthread_mutex_unlock(&stream->lock);
	return ret;
}


/**
 * Get the number of frames buffered.
 */
unsigned int
avbox_audiostream_count(struct avbox_audiostream * const stream)
{
	assert(stream != NULL);
	return avbox_queue_count(stream->packets);
}


/**
 * Check if the audio stream is paused.
 */
int
avbox_audiostream_ispaused(const struct avbox_audiostream * const stream)
{
	assert(stream != NULL);
	return stream->paused;
}


/**
 * Waits for the buffer to be empty and pauses
 * the stream.
 */
int
avbox_audiostream_drain(struct avbox_audiostream * const inst)
{
	while (avbox_queue_count(inst->packets)) {
		usleep(100L * 1000L);
	}
	return avbox_audiostream_pause(inst);
}


/**
 * Create a new sound stream
 */
struct avbox_audiostream *
avbox_audiostream_new(int max_frames,
	avbox_audiostream_callback callback,
	void * const callback_context)
{
	struct avbox_audiostream *stream;

	DEBUG_PRINT(LOG_MODULE, "Initializing audio stream");

	/* allocate stream object */
	if ((stream = malloc(sizeof(struct avbox_audiostream))) == NULL) {
		LOG_PRINT_ERROR("Could not create audio stream. Out of memory");
		/* errno = ENOMEM; */
		return NULL;
	}

	/* initialize stream object */
	memset(stream, 0, sizeof(struct avbox_audiostream));
	stream->packets = avbox_queue_new(0);
	if (stream->packets == NULL) {
		free(stream);
		return NULL;
	}

	/* initialize pthread primitives */
	if (pthread_mutex_init(&stream->lock, NULL) != 0 ||
		pthread_cond_init(&stream->wake, NULL) != 0) {
		free(stream);
		errno = EFAULT;
		return NULL;
	}

	stream->max_frames = max_frames;
	stream->queued_frames = 0;
	stream->callback = callback;
	stream->callback_context = callback_context;

	return stream;
}


/**
 * Destroy a sound stream.
 */
void
avbox_audiostream_destroy(struct avbox_audiostream * const stream)
{
	DEBUG_PRINT("audio", "Destroying audio stream");

	avbox_audiostream_drop(stream);

	/* wait for IO thread */
	pthread_mutex_lock(&stream->lock);
	if (stream->running) {
		stream->quit = 1;
		avbox_queue_close(stream->packets);
		pthread_cond_broadcast(&stream->wake);
		pthread_mutex_unlock(&stream->lock);
		pthread_join(stream->thread, 0);
		pthread_mutex_lock(&stream->lock);
		
	}
	pthread_mutex_unlock(&stream->lock);

	/* free any remaining packets */
	avbox_queue_destroy(stream->packets);

	/* free stream object */
	free(stream);
}


/**
 * Initialize audio subsystem.
 */
int
avbox_audiostream_init(void)
{
	return 0;
}


/**
 * Shutdown audio subsystem.
 */
void
avbox_audiostream_shutdown(void)
{
	snd_config_update_free_global();
}
