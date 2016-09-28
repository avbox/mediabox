#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <alsa/asoundlib.h>

#define LOG_MODULE "sound"

#include "log.h"
#include "debug.h"
#include "su.h"
#include "linkedlist.h"
#include "compiler.h"

/* we don't need all this and when we're done here
 * we won't need any of them */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>


#define MB_AUDIO_BUFFER_FRAMES  (160)


struct mb_audio_stream
{
	snd_pcm_t *pcm_handle;
	pthread_mutex_t lock;
	pthread_mutex_t queue_lock;
	pthread_cond_t wake;
	pthread_t thread;
	int quit;
	int paused;
	int frames;
	int playback_running;
	int playback_started;
	int64_t clock_offset;
	int64_t lasttime;
	int64_t underrun_time;
	snd_pcm_uframes_t buffer_size;
	unsigned int framerate;

	/* we want to get rid of these and store incoming frames
	 * on a dynamic fifo buffer */
	AVFrame *frame[MB_AUDIO_BUFFER_FRAMES];
	AVRational frame_timebase[MB_AUDIO_BUFFER_FRAMES];
	char frame_state[MB_AUDIO_BUFFER_FRAMES];
	int playback_index;
	int write_index;
};


/**
 * Flush an audio stream.
 */
void
mb_audio_stream_flush(struct mb_audio_stream * const inst)
{
	/* mb_player_flushaudio */
	pthread_mutex_lock(&inst->lock);
	while (!inst->quit) {
		/* first drain the decoded frames buffer */
		if (!inst->quit && (inst->frame_state[inst->playback_index] != 1)) {
			break;
		}

		/* free the frame */
		av_frame_free(&inst->frame[inst->playback_index]);
		inst->frame_state[inst->playback_index] = 0;
		inst->playback_index++;
		inst->playback_index %= MB_AUDIO_BUFFER_FRAMES;
		__sync_fetch_and_sub(&inst->frames, 1);
	}
	pthread_cond_signal(&inst->wake);
	pthread_mutex_unlock(&inst->lock);
}


/**
 * Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
int64_t
mb_audio_stream_gettime(struct mb_audio_stream * const inst)
{
	int err = 0;
	uint64_t time;
	snd_pcm_status_t *status;
	snd_pcm_state_t state;
	snd_htimestamp_t audio_timestamp, audio_trigger_timestamp;

	if (inst->paused) {
		return inst->clock_offset;
	}

	snd_pcm_status_alloca(&status);

	if (inst->pcm_handle == NULL || (err = snd_pcm_status(inst->pcm_handle, status)) < 0) {
		LOG_VPRINT_ERROR("Stream status error: %s", snd_strerror(err));
		return 0;
	}

	state = snd_pcm_status_get_state(status);
	snd_pcm_status_get_trigger_htstamp(status, &audio_trigger_timestamp);
	snd_pcm_status_get_htstamp(status, &audio_timestamp);

	if (state == SND_PCM_STATE_XRUN || state == SND_PCM_STATE_SETUP) {
		return inst->underrun_time;

	} else if (state != SND_PCM_STATE_RUNNING) {
		return inst->lasttime;
	}

	time  = ((audio_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_timestamp.tv_nsec) / 1000;
	time -= ((audio_trigger_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_trigger_timestamp.tv_nsec) / 1000;
	time += inst->clock_offset;

	return inst->lasttime = (int64_t) time;
}


/**
 * Pauses the audio stream and synchronizes
 * the audio clock.
 */
int
mb_audio_stream_pause(struct mb_audio_stream * const inst)
{
	int err;
	uint64_t time;
	snd_pcm_status_t *status;
	snd_pcm_state_t state;
	snd_htimestamp_t audio_timestamp, audio_trigger_timestamp;
	snd_pcm_uframes_t avail;

	DEBUG_PRINT("audio", "Pausing audio stream");

	snd_pcm_status_alloca(&status);

	pthread_mutex_lock(&inst->lock);

	if ((err = snd_pcm_status(inst->pcm_handle, status)) < 0) {
		LOG_VPRINT_ERROR("Stream status error: %s\n", snd_strerror(err));
		pthread_mutex_unlock(&inst->lock);
		return -1;
	}

	state = snd_pcm_status_get_state(status);
	snd_pcm_status_get_trigger_htstamp(status, &audio_trigger_timestamp);
	snd_pcm_status_get_htstamp(status, &audio_timestamp);

	if (state == SND_PCM_STATE_RUNNING) {
		time  = ((audio_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_timestamp.tv_nsec) / 1000;
		time -= ((audio_trigger_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_trigger_timestamp.tv_nsec) / 1000;
		time += inst->clock_offset;
	} else {
		time = inst->lasttime;
	}

	/* fprintf(stderr, "time=%lu offset=%li\n", time, inst->audio_clock_offset); */
	assert(time > 0 || inst->lasttime == 0);
	assert(time < INT64_MAX);

	/* set the clock to the current audio time */
	inst->clock_offset = time;
	inst->paused = 1;
	avail = snd_pcm_status_get_avail(status);

	/* move the clock up to what it should be after the ALSA buffer is drained
	 * and drain the buffer */
	inst->clock_offset += ((1000 * 1000) / inst->framerate) * (inst->buffer_size - avail);
	snd_pcm_drain(inst->pcm_handle);

	assert(inst->clock_offset > 0);

	pthread_mutex_unlock(&inst->lock);

	return 0;
}


/**
 * Resume audio playback
 */
int
mb_audio_stream_resume(struct mb_audio_stream * const inst)
{
	int err, ret = -1;

	DEBUG_PRINT("audio", "Resuming audio stream");

	pthread_mutex_lock(&inst->lock);

	if (!inst->paused) {
		LOG_PRINT_ERROR("Cannot resume non-paused stream");
		goto end;
	}

	assert(inst->clock_offset > 0);

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

	return ret;
}


/**
 * Calculate the next underrun time.
 */
static void
mb_audio_stream_calcunderruntime(struct mb_audio_stream * const stream)
{
	int err;
	uint64_t time;
	snd_pcm_status_t *status;

	snd_pcm_status_alloca(&status);

	/* get the pcm stream status */
	if ((err = snd_pcm_status(stream->pcm_handle, status)) < 0) {
		LOG_VPRINT_ERROR("Stream status error: %s", snd_strerror(err));
		stream->underrun_time = stream->clock_offset;
		return;
	}

	/* get the current stream time */
	time = mb_audio_stream_gettime(stream);

	/* add the time that will take to play what's left of the buffer */
	time += ((1000L * 1000L) / stream->framerate) *
		(stream->buffer_size - snd_pcm_status_get_avail(status));

	/* save it */
	stream->underrun_time = time;
}


static void
mb_audio_stream_dumpstatus(struct mb_audio_stream * const stream)
{
#ifndef NDEBUG
	int err;
	snd_pcm_state_t state;
	snd_pcm_status_t *status;
	snd_pcm_status_alloca(&status);

	/* get the pcm stream status */
	if ((err = snd_pcm_status(stream->pcm_handle, status)) < 0) {
		LOG_VPRINT_ERROR("Cannot dump ALSA status: %s\n", snd_strerror(err));
		return;
	}

	/* get the stream state */
	state = snd_pcm_status_get_state(status);

	/* print the state */
	switch (state) {
	case SND_PCM_STATE_OPEN: DEBUG_PRINT("audio", "ALSA state: OPEN"); break;
	case SND_PCM_STATE_SETUP: DEBUG_PRINT("audio", "ALSA state: SETUP"); break;
	case SND_PCM_STATE_PREPARED: DEBUG_PRINT("audio", "ALSA state: PREPARED"); break;
	case SND_PCM_STATE_RUNNING: DEBUG_PRINT("audio", "ALSA state: RUNNING"); break;
	case SND_PCM_STATE_XRUN: DEBUG_PRINT("audio", "ALSA state: XRUN"); break;
	case SND_PCM_STATE_DRAINING: DEBUG_PRINT("audio", "ALSA state: DRAINING"); break;
	case SND_PCM_STATE_PAUSED: DEBUG_PRINT("audio", "ALSA state: PAUSED"); break;
	case SND_PCM_STATE_SUSPENDED: DEBUG_PRINT("audio", "ALSA state: SUSPENDED"); break;
	case SND_PCM_STATE_DISCONNECTED: DEBUG_PRINT("audio", "ALSA state: DISCONNECTED"); break;
	default: DEBUG_VPRINT("audio", "Unknown ALSA state: %i", state); break;
	}

#endif
}


/**
 * This is the main playback loop.
 */
static void*
mb_audio_stream_io(void *arg)
{
	int ret;
	struct mb_audio_stream * const inst = (struct mb_audio_stream * const) arg;
	const char *device = "default";
	unsigned int period_usecs = 10;
	int dir;
	snd_pcm_hw_params_t *params;
	snd_pcm_sw_params_t *swparams;
	snd_pcm_sframes_t frames;
	snd_pcm_uframes_t period_frames = 8;

	MB_DEBUG_SET_THREAD_NAME("audio_playback");
	DEBUG_PRINT("player", "Audio playback thread started");

	assert(inst != NULL);
	assert(inst->pcm_handle == NULL);
	assert(inst->quit == 0);
	assert(inst->paused == 0);

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_sw_params_alloca(&swparams);

	inst->framerate = 48000;

	(void) mb_su_gainroot();

	/* initialize alsa device */
	if ((ret = snd_pcm_open(&inst->pcm_handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		LOG_VPRINT_ERROR("snd_pcm_open() failed: %s", snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_any(inst->pcm_handle, params)) < 0) {
		LOG_VPRINT_ERROR("Broken ALSA configuration: none available. %s", snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_set_access(inst->pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		LOG_VPRINT_ERROR("INTERLEAVED RW access not available. %s", snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_set_format(inst->pcm_handle, params, SND_PCM_FORMAT_S16_LE)) < 0) {
		LOG_VPRINT_ERROR("Format S16_LE not supported. %s", snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_set_channels(inst->pcm_handle, params, 2)) < 0) {
		LOG_VPRINT_ERROR("2 Channels not available. %s", snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_set_rate_near(inst->pcm_handle, params, &inst->framerate, &dir)) < 0) {
		LOG_VPRINT_ERROR("48000Hz not available. %s", snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_set_period_size_near(inst->pcm_handle, params, &period_frames, &dir)) < 0) {
		LOG_VPRINT_ERROR("Cannot set period. %s", snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params(inst->pcm_handle, params)) < 0) {
		LOG_VPRINT_ERROR("Could not set ALSA params: %s", snd_strerror(ret));
		goto audio_exit;
	}

	if ((ret = snd_pcm_sw_params_current(inst->pcm_handle, swparams)) < 0) {
		LOG_VPRINT_ERROR("Could not determine SW params. %s", snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_sw_params_set_tstamp_type(inst->pcm_handle, swparams, SND_PCM_TSTAMP_TYPE_MONOTONIC)) < 0) {
		LOG_VPRINT_ERROR("Could not set ALSA clock to CLOCK_MONOTONIC. %s", snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_sw_params(inst->pcm_handle, swparams)) < 0) {
		LOG_VPRINT_ERROR("Could not set ALSA SW paramms. %s", snd_strerror(ret));
		goto audio_exit;
	}

	if ((ret = snd_pcm_hw_params_get_period_time(params, &period_usecs, &dir)) < 0) {
		fprintf(stderr, "player: Could not get ALSA period time. %s\n",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_hw_params_get_rate(params, &inst->framerate, &dir)) < 0) {
		fprintf(stderr, "player: Could not get ALSA framerate. %s\n",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_hw_params_get_period_size(params, &period_frames, &dir)) < 0) {
		fprintf(stderr, "player: Could not get ALSA period size. %s\n",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_hw_params_get_buffer_size(params, &inst->buffer_size)) < 0) {
		fprintf(stderr, "player: Could not get ALSA buffer size. %s\n",
			snd_strerror(ret));
	}

	DEBUG_VPRINT("audio", "ALSA buffer size: %lu", (unsigned long) inst->buffer_size);
	DEBUG_VPRINT("audio", "ALSA period size: %lu", (unsigned long) period_frames);
	DEBUG_VPRINT("audio", "ALSA period time: %u", period_usecs);
	DEBUG_VPRINT("audio", "ALSA framerate: %u", inst->framerate);

	/* dump ALSA status */
	mb_audio_stream_dumpstatus(inst);

	(void) mb_su_droproot();

	/* signal video thread that we're ready to start */
	pthread_mutex_lock(&inst->lock);
	pthread_cond_signal(&inst->wake);
	pthread_mutex_unlock(&inst->lock);

	DEBUG_PRINT("audio", "Audio thread ready");

	/* start audio IO */
	while (LIKELY(inst->quit == 0)) {

		pthread_mutex_lock(&inst->lock);

		/* if there's no frame ready we must wait */
		if (UNLIKELY(inst->frame_state[inst->playback_index] != 1) || inst->paused) {
			if (inst->quit) {
				pthread_mutex_unlock(&inst->lock);
				continue;
			}
			if (LIKELY(inst->frame_state[inst->playback_index] != 1) || inst->paused) {
				DEBUG_PRINT("audio", "Audio stream stalled");
				pthread_cond_wait(&inst->wake, &inst->lock);
				pthread_mutex_unlock(&inst->lock);
				continue;
			}
		}

write_frames:
		/* play the frame */
		frames = snd_pcm_writei(inst->pcm_handle, inst->frame[inst->playback_index]->data[0],
			inst->frame[inst->playback_index]->nb_samples);
		if (UNLIKELY(frames < 0)) {
			/* dump the stream status */
			mb_audio_stream_dumpstatus(inst);

			/* we never attempt writing to a SUSPENDED stream */
			assert(frames != -ESTRPIPE);

			/* if we have underrun try to recover */
			if (frames == -EPIPE || frames == -EINTR) {
				DEBUG_PRINT("audio", "Recovering from ALSA error");
				if ((frames = snd_pcm_recover(inst->pcm_handle, frames, 0)) < 0) {
					LOG_VPRINT_ERROR("Could not recover from ALSA underrun: %s",
						snd_strerror(frames));
					av_frame_free(&inst->frame[inst->playback_index]);
					pthread_mutex_unlock(&inst->lock);
					goto audio_exit;
				}

				/* write the frames again */
				goto write_frames;
			}
		}
		if (UNLIKELY(frames < 0)) {
			LOG_VPRINT_ERROR("Unable to recover from ALSA underrun: %s",
				snd_strerror(frames));
			av_frame_free(&inst->frame[inst->playback_index]);
			pthread_mutex_unlock(&inst->lock);
			goto audio_exit;
		}

		inst->playback_started = 1;

		/* calculate the next underrun time */
		mb_audio_stream_calcunderruntime(inst);

		/* free frame */
		av_frame_free(&inst->frame[inst->playback_index]);

		/* update buffer state and signal decoder */
		inst->frame_state[inst->playback_index] = 0;
		pthread_mutex_lock(&inst->queue_lock);
		pthread_cond_signal(&inst->wake);
		pthread_mutex_unlock(&inst->queue_lock);
		pthread_mutex_unlock(&inst->lock);

		inst->playback_index = (inst->playback_index + 1) % MB_AUDIO_BUFFER_FRAMES;

		ATOMIC_DEC(&inst->frames);
	}

audio_exit:
	DEBUG_PRINT("audio", "Audio thread exiting");

	/* cleanup */
	if (inst->pcm_handle != NULL) {
		snd_pcm_hw_free(inst->pcm_handle);
		snd_pcm_close(inst->pcm_handle);
		inst->pcm_handle = NULL;
	}

	inst->playback_running = 0;

	/* signal that we're quitting */
	pthread_mutex_lock(&inst->lock);
	pthread_cond_signal(&inst->wake);
	pthread_mutex_unlock(&inst->lock);

	return NULL;
}


/**
 * Writes an audio frame to the stream.
 *
 * NOTE: This function is NOT thread safe!
 *
 * TODO: Implement this using plain buffers (not AVFrame). Problem
 * is we're using the pty.
 */
int
mb_audio_stream_write(struct mb_audio_stream *stream, AVFrame* frame, AVRational timebase)
{
	while (!stream->quit) {
		/* if the video output has not finished we must wait */
		if (UNLIKELY(stream->frame_state[stream->write_index] != 0)) {
			pthread_mutex_lock(&stream->queue_lock);
			if (UNLIKELY(stream->frame_state[stream->write_index] != 0)) {
				if (UNLIKELY(stream->quit)) {
					pthread_mutex_unlock(&stream->queue_lock);
					continue;
				}
				if (LIKELY(stream->frame_state[stream->write_index] != 0)) {
					pthread_cond_wait(&stream->wake, &stream->queue_lock);
				}
			}
			pthread_mutex_unlock(&stream->queue_lock);
			continue;
		}
		break;
	}

	stream->frame[stream->write_index] = av_frame_clone(frame);
	stream->frame_timebase[stream->write_index] = timebase;

	/* update the buffer index and signal renderer thread */
	pthread_mutex_lock(&stream->queue_lock);
	stream->frame_state[stream->write_index] = 1;
	pthread_cond_signal(&stream->wake);
	pthread_mutex_unlock(&stream->queue_lock);

	stream->write_index++;
	stream->write_index %= MB_AUDIO_BUFFER_FRAMES;

	ATOMIC_INC(&stream->frames);

	return 0;
}


/**
 * Starts the stream playback.
 */
int
mb_audio_stream_start(struct mb_audio_stream * const stream)
{
	if (!stream->playback_running) {
		pthread_mutex_lock(&stream->lock);
		if (!stream->playback_running) {
			stream->playback_running = 1;
			if (pthread_create(&stream->thread, NULL, mb_audio_stream_io, stream) != 0) {
				LOG_PRINT_ERROR("Could not start IO thread");
				stream->playback_running = 0;
				pthread_mutex_unlock(&stream->lock);
				return -1;
			}
			pthread_cond_wait(&stream->wake, &stream->lock);
		}
		pthread_mutex_unlock(&stream->lock);
		return 0;
	}
	return -1;
}


/**
 * Get the number of frames buffered.
 */
unsigned int
mb_audio_stream_getframecount(struct mb_audio_stream * const stream)
{
	assert(stream != NULL);
	return stream->frames;
}


/**
 * Check if the audio stream is paused.
 */
int
mb_audio_stream_ispaused(const struct mb_audio_stream * const stream)
{
	assert(stream != NULL);
	return stream->paused;
}


/**
 * Set the audio clock offset.
 */
int
mb_audio_stream_setclock(struct mb_audio_stream * const stream, const int64_t clock)
{
	assert(stream != NULL);
	stream->clock_offset = clock;
	stream->underrun_time = clock;
	return 0;
}


/**
 * Create a new sound stream
 */
struct mb_audio_stream *
mb_audio_stream_new(void)
{
	struct mb_audio_stream *inst;

	/* allocate stream object */
	if ((inst = malloc(sizeof(struct mb_audio_stream))) == NULL) {
		LOG_PRINT_ERROR("Could not create audio stream. Out of memory");
		errno = ENOMEM;
		return NULL;
	}

	/* initialize stream object */
	memset(inst, 0, sizeof(struct mb_audio_stream));

	/* initialize pthread primitives */
	if (pthread_mutex_init(&inst->lock, NULL) != 0 ||
		pthread_mutex_init(&inst->queue_lock, NULL) != 0 ||
		pthread_cond_init(&inst->wake, NULL) != 0) {
		free(inst);
		errno = EFAULT;
		return NULL;
	}

	return inst;
}


/**
 * Destroy a sound stream.
 */
void
mb_audio_stream_destroy(struct mb_audio_stream * const stream)
{
	int i;

	/* wait for IO thread */
	pthread_mutex_lock(&stream->lock);
	if (stream->playback_running) {
		stream->quit = 1;
		pthread_cond_signal(&stream->wake);
		pthread_mutex_unlock(&stream->lock);
		pthread_join(stream->thread, 0);
		pthread_mutex_lock(&stream->lock);
		
	}
	pthread_mutex_unlock(&stream->lock);

	/* free audio frames */
	for (i = 0; i < MB_AUDIO_BUFFER_FRAMES; i++) {
		if (stream->frame_state[i] == 1) {
			av_frame_free(&stream->frame[i]);
		}
	}

	/* free stream object */
	free(stream);
}

