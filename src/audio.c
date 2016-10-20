#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <alsa/asoundlib.h>

#define LOG_MODULE "sound"

#include "log.h"
#include "debug.h"
#include "su.h"
#include "linkedlist.h"
#include "compiler.h"
#include "time_util.h"
#include "math_util.h"


/**
 * Structure for storing audio packets.
 */
LISTABLE_STRUCT(mb_audio_packet,
	size_t n_frames;
	uint8_t *data;
);


/**
 * Audio stream structure.
 */
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
	int64_t clock_offset;
	int64_t lasttime;
	int64_t xruntime;
	snd_pcm_uframes_t buffer_size;
	unsigned int framerate;
	LIST packets;
};


/**
 * Get the size in bytes of a number of audio samples.
 */
static inline size_t
mb_audio_stream_frames2size(struct mb_audio_stream * const stream, ssize_t frames)
{
	/* For now this is hardcoded to 4 bytes per frame since we
	 * only support 16-bit stereo */
	(void) stream;
	return frames * 4;
}


/**
 * Flush the queue
 */
static void
mb_audio_stream_dropqueue(struct mb_audio_stream * const stream)
{
	struct mb_audio_packet *packet;
	pthread_mutex_lock(&stream->queue_lock);
	LIST_FOREACH_SAFE(struct mb_audio_packet *, packet, &stream->packets, {
		LIST_REMOVE(packet);
		ATOMIC_DEC(&stream->frames);
		free(packet);
	});
	pthread_mutex_unlock(&stream->queue_lock);
}


/**
 * Flush an audio stream.
 */
void
mb_audio_stream_drop(struct mb_audio_stream * const inst)
{
	/* mb_player_flushaudio */
	pthread_mutex_lock(&inst->lock);
	mb_audio_stream_dropqueue(inst);
	pthread_cond_signal(&inst->wake);
	pthread_mutex_unlock(&inst->lock);
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


static inline int64_t
mb_audio_stream_gettime_internal(struct mb_audio_stream * const inst, snd_pcm_status_t *pcm_status)
{
	uint64_t time;
	snd_pcm_state_t state;
	snd_htimestamp_t ts, tts;

	state = snd_pcm_status_get_state(pcm_status);
	snd_pcm_status_get_trigger_htstamp(pcm_status, &tts);
	snd_pcm_status_get_htstamp(pcm_status, &ts);

	switch (state) {
	case SND_PCM_STATE_XRUN:
		if (UNLIKELY(inst->lasttime != inst->xruntime)) {
			DEBUG_VPRINT("audio", "PCM State is XRUN! (clock_offset=%li xruntime=%li lasttime=%li",
				inst->clock_offset, inst->xruntime, inst->lasttime);
		}
		inst->clock_offset = inst->lasttime = inst->xruntime;
		/* fall through */
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
		time += inst->clock_offset;
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
mb_audio_stream_gettime(struct mb_audio_stream * const inst)
{
	int err = 0;
	snd_pcm_status_t *status;

	if (inst->paused) {
		return inst->clock_offset;
	}

	snd_pcm_status_alloca(&status);

	if (inst->pcm_handle == NULL || (err = snd_pcm_status(inst->pcm_handle, status)) < 0) {
		LOG_VPRINT_ERROR("Stream status error: %s", snd_strerror(err));
		return inst->lasttime;
	}

	return mb_audio_stream_gettime_internal(inst, status);
}


/**
 * Pauses the audio stream and synchronizes
 * the audio clock.
 */
int
mb_audio_stream_pause(struct mb_audio_stream * const inst)
{
	int err, ret = -1;
	snd_pcm_status_t *status;

	DEBUG_PRINT("audio", "Pausing audio stream");

	snd_pcm_status_alloca(&status);

	pthread_mutex_lock(&inst->lock);

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
		DEBUG_VPRINT("audio", "Pausing on XRUN: offset=%li xruntime=%li lasttime=%li",
			inst->clock_offset, inst->xruntime, inst->lasttime);
		inst->paused = 1;
		inst->clock_offset = inst->xruntime;
		snd_pcm_drain(inst->pcm_handle);
		ret = 0;
		goto end;
	case SND_PCM_STATE_RUNNING:
		DEBUG_VPRINT("audio", "Pausing RUNNING stream (offset=%li,time=%li)",
			inst->clock_offset, mb_audio_stream_gettime(inst));

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

		/* make sure we return the right time while paused */
		inst->lasttime = inst->clock_offset = inst->xruntime;

		/* dump the stream status */
		mb_audio_stream_dumpstatus(inst);

		ret = 0;
		goto end;
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
mb_audio_stream_resume(struct mb_audio_stream * const inst)
{
	int err, ret = -1;

	DEBUG_VPRINT("audio", "Resuming audio stream (time=%li)",
		mb_audio_stream_gettime(inst));

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
	DEBUG_VPRINT("audio", "Audio stream resumed (time=%li)",
		mb_audio_stream_gettime(inst));

	/* signal IO thread */
	pthread_cond_signal(&inst->wake);
	pthread_mutex_unlock(&inst->lock);

	return ret;
}


/**
 * Calculate the next underrun time.
 */
static void
mb_audio_stream_calcxruntime(struct mb_audio_stream * const stream)
{
	int err;
	uint64_t time;
	snd_pcm_status_t *status;

	snd_pcm_status_alloca(&status);

	/* get the pcm stream status */
	if ((err = snd_pcm_status(stream->pcm_handle, status)) < 0) {
		LOG_VPRINT_ERROR("Stream status error: %s", snd_strerror(err));
		stream->xruntime = stream->clock_offset;
		return;
	}

	/* get the current stream time */
	time = mb_audio_stream_gettime_internal(stream, status);

	/* add the time that will take to play what's left of the buffer */
	time += ((stream->buffer_size - snd_pcm_status_get_avail(status)) * 1000L * 1000L) /
		stream->framerate;

	/* save it */
	stream->xruntime = time;
}


/**
 * This is the main playback loop.
 */
static void*
mb_audio_stream_output(void *arg)
{
	int ret;
	size_t n_frames;
	struct mb_audio_stream * const inst = (struct mb_audio_stream * const) arg;
	struct mb_audio_packet * packet;
	const char *device = "default";
	unsigned int period_usecs = 10;
	int dir;
	snd_pcm_hw_params_t *params;
	snd_pcm_sw_params_t *swparams;
	snd_pcm_sframes_t frames;
	snd_pcm_uframes_t period_frames = 8, frames_write_max = 8;

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

	frames_write_max = period_frames / 2;

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

	/* set the handle to nonblock mode */
	if (snd_pcm_nonblock(inst->pcm_handle, 1) < 0) {
		LOG_PRINT_ERROR("Could not set ALSA PCM handle to nonblock mode");
	}

	/* start audio IO */
	while (LIKELY(!inst->quit)) {

		pthread_mutex_lock(&inst->lock);

		/* sleep if there are no packets or we're paused */
		if (LIST_EMPTY(&inst->packets) || inst->paused) {
			if (inst->quit) {
				pthread_mutex_unlock(&inst->lock);
				continue;
			}
			pthread_cond_wait(&inst->wake, &inst->lock);
			pthread_mutex_unlock(&inst->lock);
			continue;
		}

		/* get the next packet */
		packet = LIST_TAIL(struct mb_audio_packet*, &inst->packets);
		assert(!LIST_ISNULL(&inst->packets, packet));

		/* calculate the number of frames to write */
		n_frames = MIN(frames_write_max, packet->n_frames);

		/* play the frame */
		frames = snd_pcm_writei(inst->pcm_handle, packet->data, n_frames);
		if (UNLIKELY(frames < 0)) {
			if (UNLIKELY(frames == -EAGAIN)) {
				/* the ring buffer is full so we sleep for as long as it
				 * takes to play the current fragment to ensure there's enough
				 * room next time we try. Since the max fragment size is half
				 * the buffer size this will ensure the buffer never gets much
				 * more than half empty */
				const int64_t waitusecs =
					(n_frames * 1000L * 1000L) / inst->framerate;
				struct timespec waittime = { .tv_sec = 0, .tv_nsec = 0 };
				timeaddu(&waittime, waitusecs);
				delay2abstime(&waittime);
				pthread_cond_timedwait(&inst->wake, &inst->lock, &waittime);
				pthread_mutex_unlock(&inst->lock);
				continue;
			}

			/* dump the stream status */
			mb_audio_stream_dumpstatus(inst);

			/* if we have underrun try to recover */
			if (LIKELY(frames == -EPIPE || frames == -EINTR || frames == -ESTRPIPE)) {
				DEBUG_VPRINT("audio", "Recovering from ALSA error: %s",
					snd_strerror(frames));

				inst->clock_offset = inst->xruntime;

				if ((frames = snd_pcm_recover(inst->pcm_handle, frames, 0)) < 0) {
					LOG_VPRINT_ERROR("Could not recover from ALSA underrun: %s",
						snd_strerror(frames));
					free(packet);
					pthread_mutex_unlock(&inst->lock);
					goto audio_exit;
				}
				pthread_mutex_unlock(&inst->lock);
				continue;
			}
		}
		if (UNLIKELY(frames < 0)) {
			LOG_VPRINT_ERROR("Unable to recover from ALSA underrun: %s",
				snd_strerror(frames));
			free(packet);
			pthread_mutex_unlock(&inst->lock);
			goto audio_exit;
		}

		/* calculate the next underrun time */
		mb_audio_stream_calcxruntime(inst);

		/* unlock stream */
		pthread_mutex_unlock(&inst->lock);

		/* update the packet structure */
		packet->n_frames -= n_frames;
		packet->data += mb_audio_stream_frames2size(inst, n_frames);

		/* if there's no samples left in the packet then
		 * remove it from the queue and free it */
		if (packet->n_frames == 0) {
			pthread_mutex_lock(&inst->queue_lock);
			LIST_REMOVE(packet);
			pthread_mutex_unlock(&inst->queue_lock);
			ATOMIC_DEC(&inst->frames);
			free(packet);
		}
	}

audio_exit:
	DEBUG_PRINT("audio", "Audio thread exiting");

	/* cleanup */
	if (inst->pcm_handle != NULL) {
		snd_pcm_hw_free(inst->pcm_handle);
		snd_pcm_close(inst->pcm_handle);
		inst->pcm_handle = NULL;
	}

	/* free any remaining packets */
	mb_audio_stream_dropqueue(inst);

	inst->playback_running = 0;

	/* signal that we're quitting */
	pthread_mutex_lock(&inst->lock);
	pthread_cond_signal(&inst->wake);
	pthread_mutex_unlock(&inst->lock);

	return NULL;
}


/**
 * Writes n_frames audio frames to the stream.
 */
int
mb_audio_stream_write(struct mb_audio_stream * const stream,
	const uint8_t * const data, const size_t n_frames)
{
	struct mb_audio_packet *packet;
	const int sz = mb_audio_stream_frames2size(stream, n_frames);

	assert(stream != NULL);

	if (n_frames == 0) {
		return 0;
	}

	/* allocate memory for packet */
	if ((packet = malloc(sizeof(struct mb_audio_packet) + sz)) == NULL) {
		LOG_PRINT_ERROR("Could not allocate packet");
		errno = ENOMEM;
		return -1;
	}

	/* copy samples */
	packet->n_frames = n_frames;
	packet->data = (uint8_t*) (packet + 1);
	memcpy(packet->data, data, sz);

	/* add packet to queue */
	pthread_mutex_lock(&stream->queue_lock);
	LIST_ADD(&stream->packets, packet);
	ATOMIC_INC(&stream->frames);
	pthread_mutex_unlock(&stream->queue_lock);

	/* signal output thread */
	pthread_mutex_lock(&stream->lock);
	pthread_cond_signal(&stream->wake);
	pthread_mutex_unlock(&stream->lock);

	return 0;
}


/**
 * Starts the stream playback.
 */
int
mb_audio_stream_start(struct mb_audio_stream * const stream)
{
	int ret = -1;

	if (stream->playback_running) {
		LOG_PRINT_ERROR("Audio stream already started");
		return -1;
	}

	pthread_mutex_lock(&stream->lock);

	if (!stream->playback_running) {
		stream->playback_running = 1;
		if (pthread_create(&stream->thread, NULL, mb_audio_stream_output, stream) != 0) {
			LOG_PRINT_ERROR("Could not start IO thread");
			stream->playback_running = 0;
			goto end;
		}

		pthread_cond_wait(&stream->wake, &stream->lock);

		if (!stream->playback_running) {
			LOG_PRINT_ERROR("Audio thread initialization failed");
			goto end;
		}

		ret = 0;
	}
end:
	pthread_mutex_unlock(&stream->lock);
	return ret;
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
	stream->xruntime = clock;
	stream->lasttime = clock;
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
	LIST_INIT(&inst->packets);

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

	/* free any remaining packets */
	mb_audio_stream_dropqueue(stream);

	/* free stream object */
	free(stream);
}

