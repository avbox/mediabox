#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
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


/**
 * Structure for storing audio packets.
 */
LISTABLE_STRUCT(avbox_audio_packet,
	size_t n_frames;
	uint8_t *data;
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
	int64_t frames;
	int64_t clock_offset_internal;
	int64_t clock_offset;
	int64_t lasttime;
	snd_pcm_uframes_t buffer_size;
	unsigned int framerate;
	struct avbox_queue *packets;
};


/**
 * Calculate the amount of time (in useconds) that it would
 * take to play a given amount of frames
 */
#define FRAMES2TIME(stream, frames)	(((frames) * 1000L * 1000L) / stream->framerate)


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


/**
 * Flush the queue
 */
static void
avbox_audiostream_dropqueue(struct avbox_audiostream * const stream)
{
	DEBUG_PRINT("audio", "Dropping queue");
	struct avbox_audio_packet *packet;
	while (avbox_queue_count(stream->packets) > 0) {
		packet = avbox_queue_get(stream->packets);
		assert(packet != NULL);
		free(packet);
	}
}


/**
 * Flush an audio stream.
 */
void
avbox_audiostream_drop(struct avbox_audiostream * const inst)
{
	pthread_mutex_lock(&inst->lock);
	avbox_audiostream_dropqueue(inst);
	pthread_cond_signal(&inst->wake);
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


static inline int64_t
avbox_audiostream_gettime_internal(struct avbox_audiostream * const inst, snd_pcm_status_t *pcm_status)
{
	int64_t time;
	snd_pcm_state_t state;
	snd_htimestamp_t ts, tts;

	state = snd_pcm_status_get_state(pcm_status);
	snd_pcm_status_get_trigger_htstamp(pcm_status, &tts);
	snd_pcm_status_get_htstamp(pcm_status, &ts);

	switch (state) {
	case SND_PCM_STATE_XRUN:
	{
		const int64_t xruntime = inst->clock_offset +
			FRAMES2TIME(inst, inst->frames);
		if (UNLIKELY(inst->lasttime != xruntime)) {
			DEBUG_VPRINT("audio", "PCM State is XRUN! (clock_offset=%li xruntime=%li lasttime=%li",
				inst->clock_offset_internal, xruntime, inst->lasttime);
		}
		inst->lasttime = xruntime;
		/* fall through */
	}
	case SND_PCM_STATE_OPEN:
	case SND_PCM_STATE_SETUP:
	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_PAUSED:
	case SND_PCM_STATE_SUSPENDED:
	case SND_PCM_STATE_DISCONNECTED:
		return inst->lasttime;
	case SND_PCM_STATE_RUNNING:
	case SND_PCM_STATE_DRAINING:
		time  = ((ts.tv_sec * 1000L * 1000L * 1000L) + ts.tv_nsec) / 1000L;
		time -= ((tts.tv_sec * 1000L * 1000L * 1000L) + tts.tv_nsec) / 1000L;
		time += inst->clock_offset_internal;
		return inst->lasttime = (int64_t) time;
	default:
		DEBUG_VPRINT("audio", "Unknown ALSA state (state=%i)", state);
		return inst->lasttime;
	}
}


/**
 * Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
int64_t
avbox_audiostream_gettime(struct avbox_audiostream * const stream)
{
	int err = 0;
	int64_t ret;
	snd_pcm_status_t *status;

	/* if the stream is pause or hasn't started return
	 * the internal offset */
	if (stream->paused || stream->frames == 0) {
		ret = stream->clock_offset_internal;
		goto end;
	}

	/* if we're running we must have a pcm handle */
	assert(stream->pcm_handle);

	/* make sure we're not in xrun */
	if ((err = snd_pcm_avail(stream->pcm_handle)) < 0) {
		LOG_VPRINT_ERROR("avbox_audiostream_gettime(): ALSA error detected: %s",
			snd_strerror(err));
		ret = stream->lasttime;
		goto end;
	}

	/* get the status */
	snd_pcm_status_alloca(&status);
	if ((err = snd_pcm_status(stream->pcm_handle, status)) < 0) {
		LOG_VPRINT_ERROR("Stream status error: %s", snd_strerror(err));
		ret = stream->lasttime;
		goto end;
	}

	/* calculate stream time */
	ret = avbox_audiostream_gettime_internal(stream, status);
end:
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
		LOG_PRINT_ERROR("Error: Non-pausable state");
		ret = -1;
		goto end;
	case SND_PCM_STATE_SUSPENDED:
	case SND_PCM_STATE_PAUSED:
		DEBUG_PRINT("audio", "Unexpected ALSA state");
		abort();
		break;
	case SND_PCM_STATE_XRUN:
	{
		const int64_t xruntime = inst->clock_offset +
			FRAMES2TIME(inst, inst->frames);
		DEBUG_VPRINT("audio", "Pausing on XRUN: offset=%li xruntime=%li lasttime=%li",
			inst->clock_offset_internal, xruntime, inst->lasttime);
		inst->paused = 1;
		ret = 0;
		goto end;
	}
	case SND_PCM_STATE_RUNNING:
	{
		DEBUG_VPRINT("audio", "Pausing RUNNING stream (offset=%li,time=%li)",
			inst->clock_offset_internal, avbox_audiostream_gettime_internal(inst, status));

		/* start draining the buffer */
		inst->paused = 1;
		snd_pcm_drain(inst->pcm_handle);

		/* wait for the buffer to drain. if this fails for some
		 * reason return success */
		do {
			/* get pcm handle status */
			if ((err = snd_pcm_status(inst->pcm_handle, status)) < 0) {
				LOG_VPRINT_ERROR("Stream status error: %s\n", snd_strerror(err));
				ret =  0;
				goto end;
			}
		} while (snd_pcm_status_get_state(status) == SND_PCM_STATE_DRAINING);

		DEBUG_VPRINT("audio", "PCM state after pause: %s",
			avbox_pcm_state_getstring(snd_pcm_state(inst->pcm_handle)));

		/* make sure we return the right time while paused */
		inst->lasttime = inst->clock_offset_internal =
			inst->clock_offset + FRAMES2TIME(inst, inst->frames);

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
	int err, ret = -1;

	DEBUG_VPRINT("audio", "Resuming audio stream (time=%li)",
		avbox_audiostream_gettime(inst));

	pthread_mutex_lock(&inst->lock);

	if (!inst->paused) {
		LOG_PRINT_ERROR("Cannot resume non-paused stream");
		goto end;
	}

	/* reset ALSA. The clock will start running again when the
	 * audio starts playing */
	if ((err = snd_pcm_reset(inst->pcm_handle)) < 0) {
		LOG_VPRINT_ERROR("Could not resume playback: %s",
			snd_strerror(err));
		goto end;
	}
	if ((err = snd_pcm_prepare(inst->pcm_handle)) < 0) {
		LOG_VPRINT_ERROR("Could not resume playback: %s",
			snd_strerror(err));
		goto end;
	}

	inst->paused = 0;
	ret = 0;
end:
	/* signal IO thread */
	pthread_cond_signal(&inst->wake);
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
	int ret;
	size_t n_frames;
	struct avbox_audiostream * const inst = (struct avbox_audiostream * const) arg;
	struct avbox_audio_packet * packet;
	const char *device = "sysdefault";
	unsigned int period_usecs = 10;
	int dir;
	snd_pcm_hw_params_t *params;
	snd_pcm_sw_params_t *swparams;
	snd_pcm_sframes_t frames;
	snd_pcm_uframes_t period = 1024,
		start_thres, stop_thres, silen_thres;
	const snd_pcm_uframes_t fragment = 256;

	MB_DEBUG_SET_THREAD_NAME("audio_playback");
	DEBUG_PRINT("player", "Audio playback thread started");

	assert(inst != NULL);
	assert(inst->pcm_handle == NULL);
	assert(inst->quit == 0);
	assert(inst->paused == 0);

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_sw_params_alloca(&swparams);

	inst->framerate = 48000;

	(void) avbox_gainroot();

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
	DEBUG_VPRINT("audio", "ALSA status: %s",
		avbox_pcm_state_getstring(snd_pcm_state(inst->pcm_handle)));
	DEBUG_VPRINT("audio", "Stream offset: %lu", inst->clock_offset_internal);

	/* drop superuser privileges */
	(void) avbox_droproot();

	/* signal that we've started successfully */
	inst->running = 1;
	pthread_mutex_lock(&inst->lock);
	pthread_cond_signal(&inst->wake);
	pthread_mutex_unlock(&inst->lock);

	DEBUG_PRINT("audio", "Audio thread ready");

	/* start audio IO */
	while (1) {

		/* get the next packet */
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

		pthread_mutex_lock(&inst->lock);

		/* check if we're paused */
		if (UNLIKELY(inst->quit)) {
			pthread_mutex_unlock(&inst->lock);
			goto end;
		} else if (UNLIKELY(inst->paused)) {
			pthread_cond_wait(&inst->wake, &inst->lock);
			pthread_mutex_unlock(&inst->lock);
			continue;
		}

		/* calculate the number of frames to use from this packet */
		n_frames = MIN(fragment, packet->n_frames);

		/* write fragment to ring buffer */
		if (UNLIKELY((frames = snd_pcm_writei(inst->pcm_handle, packet->data, n_frames)) < 0)) {
			if (UNLIKELY(frames == -EAGAIN)) {
				LOG_PRINT_ERROR("Could not write frames: EAGAIN!");
				pthread_mutex_unlock(&inst->lock);
				continue;

			} else  if (LIKELY(frames == -EPIPE || frames == -EINTR || frames == -ESTRPIPE)) {
				DEBUG_VPRINT("audio", "Recovering from ALSA error: %s",
					snd_strerror(frames));

				/* update the offset */
				inst->clock_offset_internal = inst->clock_offset +
					FRAMES2TIME(inst, inst->frames);

				DEBUG_VPRINT("audio", "Setting offset to %li",
					inst->clock_offset_internal);

				if ((frames = snd_pcm_recover(inst->pcm_handle, frames, 1)) < 0) {
					LOG_VPRINT_ERROR("Could not recover from ALSA underrun: %s",
						snd_strerror(frames));
					goto end;
				}

				assert(frames == 0);
				pthread_mutex_unlock(&inst->lock);
				continue;

			} else {
				LOG_VPRINT_ERROR("Could not write audio frames: %s",
					snd_strerror(frames));
				pthread_mutex_unlock(&inst->lock);
				goto end;
			}
		}

		/* If we did a partial write print a debug message.
		 * I understand this can happen but never seen it */
		if (UNLIKELY(frames < n_frames)) {
			DEBUG_VPRINT("audio", "Only %d out of %d frames written",
				frames, n_frames);
		}

		/* update frame counts */
		inst->frames += frames;
		packet->data += avbox_audiostream_frames2size(inst, frames);
		packet->n_frames -= n_frames;

		/* if there's no frames left in the packet then
		 * remove it from the queue and free it */
		if (packet->n_frames == 0) {
			if (avbox_queue_get(inst->packets) != packet) {
				abort();
			}
			free(packet);
		}
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

	/* free any remaining packets */
	avbox_audiostream_dropqueue(inst);

	/* signal that we're quitting */
	inst->running = 0;
	pthread_cond_signal(&inst->wake);
	pthread_mutex_unlock(&inst->lock);

	return NULL;
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

	assert(stream != NULL);

	if (n_frames == 0) {
		return 0;
	}

	/* allocate memory for packet */
	if ((packet = malloc(sizeof(struct avbox_audio_packet) + sz)) == NULL) {
		LOG_PRINT_ERROR("Could not allocate packet");
		errno = ENOMEM;
		return -1;
	}

	/* copy samples */
	packet->n_frames = n_frames;
	packet->data = (uint8_t*) (packet + 1);
	memcpy(packet->data, data, sz);

	/* add packet to queue */
	if (avbox_queue_put(stream->packets, packet) == -1) {
		LOG_VPRINT_ERROR("Could not add packet to queue: %s",
			strerror(errno));
		return -1;
	}

	return 0;
}


/**
 * Starts the stream playback.
 */
int
avbox_audiostream_start(struct avbox_audiostream * const stream)
{
	int ret = -1;

	pthread_mutex_lock(&stream->lock);

	if (stream->started) {
		LOG_PRINT_ERROR("Audio stream already started");
		goto end;
	}

	if (pthread_create(&stream->thread, NULL, avbox_audiostream_output, stream) != 0) {
		LOG_PRINT_ERROR("Could not start IO thread");
		goto end;
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
 * Set the audio clock offset.
 */
int
avbox_audiostream_setclock(struct avbox_audiostream * const stream, const int64_t clock)
{
	int ret = -1;

	DEBUG_VPRINT("audio", "Setting stream (%p) clock to %li",
		stream, clock);
	assert(stream != NULL);
	assert(clock >= 0);

	pthread_mutex_lock(&stream->lock);

	/* we cannot set clock while stream is playing */
	if (stream->frames && !stream->paused) {
		LOG_PRINT_ERROR("Could not set clock! Stream running.");
		errno = EINVAL;
		goto end;
	}

	stream->clock_offset_internal =
		stream->clock_offset =
		stream->lasttime = clock;
	stream->frames = ret = 0;
end:
	pthread_mutex_unlock(&stream->lock);
	return ret;
}


/**
 * Create a new sound stream
 */
struct avbox_audiostream *
avbox_audiostream_new(void)
{
	struct avbox_audiostream *stream;

	/* allocate stream object */
	if ((stream = malloc(sizeof(struct avbox_audiostream))) == NULL) {
		LOG_PRINT_ERROR("Could not create audio stream. Out of memory");
		/* errno = ENOMEM; */
		return NULL;
	}

	/* initialize stream object */
	memset(stream, 0, sizeof(struct avbox_audiostream));
	stream->packets = avbox_queue_new(0);

	/* initialize pthread primitives */
	if (pthread_mutex_init(&stream->lock, NULL) != 0 ||
		pthread_cond_init(&stream->wake, NULL) != 0) {
		free(stream);
		errno = EFAULT;
		return NULL;
	}

	return stream;
}


/**
 * Destroy a sound stream.
 */
void
avbox_audiostream_destroy(struct avbox_audiostream * const stream)
{
	DEBUG_PRINT("audio", "Destroying audio stream");

	/* wait for IO thread */
	pthread_mutex_lock(&stream->lock);
	if (stream->running) {
		stream->quit = 1;
		avbox_queue_wake(stream->packets);
		pthread_cond_signal(&stream->wake);
		pthread_mutex_unlock(&stream->lock);
		pthread_join(stream->thread, 0);
		pthread_mutex_lock(&stream->lock);
		
	}
	pthread_mutex_unlock(&stream->lock);

	/* free any remaining packets */
	avbox_audiostream_dropqueue(stream);

	/* free stream object */
	free(stream);
}
