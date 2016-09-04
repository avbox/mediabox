#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#define HAVE_MALLOC_TRIM	(1)	/* TODO: Check for this on configure */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

#include <pango/pangocairo.h>


/* for direct rendering */
#include <linux/fb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>


#include <alsa/asoundlib.h>


#include "video.h"
#include "player.h"
#include "debug.h"
#include "su.h"
#include "time_util.h"
#include "timers.h"


/*
 * For now to change the pix format it needs to be
 * done here and on video-directfb.c mbv_window_blit_buffer()
 * function. We need to implement our own enum with supported
 * formats (on video.h) and add it as an argument to that
 * function. Then use a LUT to map between those and ffmpeg's.
 */
//#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_RGB565)
//#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_RGB32)
#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_BGRA)

/* This is the # of frames to decode ahead of time */
#define MB_VIDEO_BUFFER_FRAMES  (50)
#define MB_VIDEO_BUFFER_PACKETS (1)
#define MB_AUDIO_BUFFER_FRAMES  (160)
#define MB_AUDIO_BUFFER_PACKETS (1)

#define MB_DECODER_PRINT_FPS
/* #define ENABLE_DOUBLE_BUFFERING (1) */

/* render directly to fbdev */
#define MB_FBDEV_RENDERER

#define MB_ALSA_BUFFER_SIZE	(32 * 1024)
#define MB_ALSA_LATENCY		(500000)

/* Macros for optimizing likely branches */
#define LIKELY(x)               (__builtin_expect(!!(x), 1))
#define UNLIKELY(x)             (__builtin_expect(!!(x), 0))

struct mbp
{
	struct mbv_window *window;
	const char *media_file;
	enum mb_player_status status;
	int frames_rendered;
	int width;
	int height;
	int last_err;
	int have_audio;
	int have_video;
	int stream_quit;
	int64_t seek_to;
	int seek_result;
	uint8_t *buf;
	int bufsz;
	uint8_t *render_mask;
	int use_fbdev;
	struct timespec systemreftime;
	int64_t lasttime;
	int64_t systemtimeoffset;
	int64_t (*getmastertime)(struct mbp *inst);
	mb_player_status_callback status_callback;

	AVFormatContext *fmt_ctx;

	snd_pcm_t *audio_pcm_handle;
	AVFrame *audio_frame[MB_AUDIO_BUFFER_FRAMES];
	AVRational audio_frame_timebase[MB_AUDIO_BUFFER_FRAMES];
	char audio_frame_state[MB_AUDIO_BUFFER_FRAMES];
	int audio_playback_index;
	int audio_decode_index;
	int audio_frames;
	int audio_packets;
	int audio_pause_requested;
	int audio_quit;
	int audio_paused;
	int audio_playback_running;
	int64_t audio_clock_offset;
	snd_pcm_uframes_t audio_buffer_size; /* ALSA buffer */
	unsigned int audio_framerate;
	pthread_cond_t audio_signal;
	pthread_mutex_t audio_lock;
	pthread_t audio_thread;

	int audio_decoder_quit;
	AVPacket audio_packet[MB_AUDIO_BUFFER_PACKETS];
	char audio_packet_state[MB_AUDIO_BUFFER_PACKETS];
	int audio_packet_read_index;
	int audio_packet_write_index;

	int audio_stream_index;
	pthread_cond_t audio_decoder_signal;
	pthread_mutex_t audio_decoder_lock;
	pthread_t audio_decoder_thread;

	int video_stream_index;
	AVCodecContext *video_codec_ctx;
	AVPacket video_packet[MB_VIDEO_BUFFER_PACKETS];
	char video_packet_state[MB_VIDEO_BUFFER_PACKETS];
	void *video_last_frame;
#ifdef ENABLE_DOUBLE_BUFFERING
	void *video_buffer;
#endif
	uint8_t frame_repeat[MB_VIDEO_BUFFER_FRAMES];
	uint8_t *frame_data[MB_VIDEO_BUFFER_FRAMES];
	char frame_state[MB_VIDEO_BUFFER_FRAMES];
	int64_t frame_pts[MB_VIDEO_BUFFER_FRAMES];
	AVRational frame_time_base[MB_VIDEO_BUFFER_FRAMES];
	int video_frames;
	int video_packets;
	int video_playback_index;
	int video_decode_index;
	int video_quit;
	int video_decoder_quit;
	int video_packet_read_index;
	int video_packet_write_index;
	int video_paused;
	int video_playback_running;
	unsigned int video_skipframes;
	int64_t video_decoder_pts;
	AVRational video_decoder_timebase;
	pthread_cond_t video_decoder_signal;
	pthread_mutex_t video_decoder_lock;
	pthread_t video_decoder_thread;
	pthread_cond_t video_output_signal;
	pthread_mutex_t video_output_lock;
	pthread_t video_output_thread;

	pthread_cond_t resume_signal;
	pthread_mutex_t resume_lock;
	pthread_t thread;

	int stream_percent;

	int top_overlay_timer_id;
	char *top_overlay_text;
	enum mbv_alignment top_overlay_alignment;
	pthread_mutex_t top_overlay_lock;
};


/* Pango global context */
PangoFontDescription *pango_font_desc = NULL;


/**
 * mb_player_updatestatus() -- Updates the player status and
 * calls any registered callbacks
 */
static void
mb_player_updatestatus(struct mbp *inst, enum mb_player_status status)
{
	enum mb_player_status last_status;

	assert(inst != NULL);

	last_status = inst->status;
	inst->status = status;

	/* invoke the status callback */
	if (inst->status_callback != NULL) {
		inst->status_callback(inst, status, last_status);
	}
}


static inline void
mb_player_printstatus(struct mbp *inst, int fps)
{
	static int i = 0;
	if ((i++ % 10) == 0) {
		fprintf(stdout, "| Fps: %03i | Video Packets: %03i | Video Frames: %03i | Audio Packets: %03i | Audio Frames: %03i |\r",
			fps, inst->video_packets, inst->video_frames, inst->audio_packets, inst->audio_frames);
		fflush(stdout);
	}
}


/**
 * mb_player_rendertext() -- Renders a text overlay on top of
 * the video image
 */
static int
mb_player_rendertext(struct mbp *inst, cairo_t *context, char *text, PangoRectangle *rect)
{
	PangoLayout *layout;

	assert(inst != NULL);
	assert(context != NULL);
	assert(text != NULL);
	assert(rect != NULL);

	cairo_translate(context, rect->x, rect->y);

	if ((layout = pango_cairo_create_layout(context)) != NULL) {
		pango_layout_set_font_description(layout, pango_font_desc);
		pango_layout_set_width(layout, rect->width * PANGO_SCALE);
		pango_layout_set_height(layout, 400 * PANGO_SCALE);
		pango_layout_set_alignment(layout, mbv_get_pango_alignment(inst->top_overlay_alignment));
		pango_layout_set_text(layout, text, -1);



		cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
		pango_cairo_update_layout(context, layout);
		pango_cairo_show_layout(context, layout);

		g_object_unref(layout);
	}

	return 0;
}


/**
 * mb_player_dumpvideo() -- Dump all video frames up to the specified
 * pts (in usecs)
 *
 * WARNING: DO NOT call this function from any thread except the
 * video output thread.
 */
static inline int
mb_player_dumpvideo(struct mbp* inst, int flush)
{
	int ret = 0;
	int64_t video_time, pts;

	fprintf(stderr, "player: Skipping frames\n");

	/*
	pthread_mutex_lock(&inst->video_output_lock);
	video_time = av_rescale_q(inst->video_decoder_pts,
		inst->video_decoder_timebase, AV_TIME_BASE_Q);
	pthread_mutex_unlock(&inst->video_output_lock);
	*/
	pts = inst->getmastertime(inst);
	video_time = pts - 10000 - 1;

	while (!inst->video_quit && (flush || video_time < (pts - 10000))) {
		/* tell decoder to skip frames */
		inst->video_codec_ctx->skip_frame = AVDISCARD_NONREF;

		/* first drain the decoded frames buffer */
		if (!inst->video_quit && (inst->frame_state[inst->video_playback_index] != 1)) {
			if (flush) {
				break;
			}
			pthread_mutex_lock(&inst->video_output_lock);
			if (!inst->video_quit && (inst->frame_state[inst->video_playback_index] != 1)) {
				pthread_cond_wait(&inst->video_output_signal, &inst->video_output_lock);
			}
			pthread_mutex_unlock(&inst->video_output_lock);
			continue;
		}

		video_time = av_rescale_q(inst->frame_pts[inst->video_playback_index],
			inst->frame_time_base[inst->video_playback_index], AV_TIME_BASE_Q);
		if (pts != -1 && video_time >= (pts - 10000)) {
			pthread_mutex_unlock(&inst->video_output_lock);
			goto end;
		}

		fprintf(stderr, "player: video_time=%li, pts=%li\n",
			video_time, pts);

		pthread_mutex_lock(&inst->video_output_lock);
		inst->frame_state[inst->video_playback_index] = 0;
		pthread_cond_signal(&inst->video_output_signal);
		pthread_mutex_unlock(&inst->video_output_lock);
		inst->video_playback_index++;
		inst->video_playback_index %= MB_VIDEO_BUFFER_FRAMES;

		pts = inst->getmastertime(inst);

		/* inst->video_frames--; */
		__sync_fetch_and_sub(&inst->video_frames, 1);
		ret = 1;
	}
end:
	inst->video_codec_ctx->skip_frame = AVDISCARD_DEFAULT;
	return ret;
}

static void
mb_player_flushaudio(struct mbp *inst)
{
	pthread_mutex_lock(&inst->audio_lock);
	//pthread_mutex_lock(&inst->audio_decoder_lock);
	while (!inst->audio_quit) {
		/* first drain the decoded frames buffer */
		if (!inst->audio_quit && (inst->audio_frame_state[inst->audio_playback_index] != 1)) {
			break;
		}

		inst->audio_frame_state[inst->audio_playback_index] = 0;
		inst->audio_playback_index++;
		inst->audio_playback_index %= MB_AUDIO_BUFFER_FRAMES;
		__sync_fetch_and_sub(&inst->video_frames, 1);
	}
	//while (!inst->audio_quit) {
	//	if (!inst->audio_quit && (inst->audio_packet_state[inst->audio_decode_index]
	//}
	pthread_cond_signal(&inst->audio_signal);
	pthread_mutex_unlock(&inst->audio_lock);


}

/**
 * mb_player_wait4buffers() -- Waits for the decoded stream buffers
 * to fill up
 */
static void
mb_player_wait4buffers(struct mbp *inst, int *quit)
{
	/* fprintf(stderr, "Buffering\n"); */
	/* wait for the buffers to fill up */
	do {
		/* mb_player_dumpvideo(inst, inst->getmastertime(inst)); */
		pthread_cond_broadcast(&inst->video_decoder_signal);
		pthread_cond_broadcast(&inst->audio_decoder_signal);
		pthread_cond_broadcast(&inst->video_output_signal);
		pthread_cond_broadcast(&inst->audio_signal);

		mb_player_printstatus(inst, 0);
		usleep(5000); /* TODO: make this interruptible */
	}
	while (!*quit && inst->video_frames < MB_VIDEO_BUFFER_FRAMES &&
		inst->audio_frames < MB_VIDEO_BUFFER_FRAMES);


}


/**
 * mb_player_getaudiotime() -- Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
static int64_t
mb_player_getaudiotime(struct mbp* inst)
{
	int err = 0;
	uint64_t time;
	snd_pcm_status_t *status;
	snd_pcm_state_t state;
	snd_htimestamp_t audio_timestamp, audio_trigger_timestamp;

	if (inst->audio_paused) {
		return inst->audio_clock_offset;
	}

	snd_pcm_status_alloca(&status);

	if (inst->audio_pcm_handle == NULL || (err = snd_pcm_status(inst->audio_pcm_handle, status)) < 0) {
		printf("Stream status error: %s\n", snd_strerror(err));
		return 0;
	}

	state = snd_pcm_status_get_state(status);
	snd_pcm_status_get_trigger_htstamp(status, &audio_trigger_timestamp);
	snd_pcm_status_get_htstamp(status, &audio_timestamp);

	if (state != SND_PCM_STATE_RUNNING) {
		return inst->lasttime;
	}

	time  = ((audio_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_timestamp.tv_nsec) / 1000;
	time -= ((audio_trigger_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_trigger_timestamp.tv_nsec) / 1000;
	time += inst->audio_clock_offset;

	return inst->lasttime = (int64_t) time;
}


static void
mb_player_resetsystemtime(struct mbp *inst, int64_t upts)
{
	(void) clock_gettime(CLOCK_MONOTONIC, &inst->systemreftime);
	inst->systemtimeoffset = upts;
}


static int64_t
mb_player_getsystemtime(struct mbp *inst)
{
	struct timespec tv;
	if (UNLIKELY(inst->video_paused)) {
		return inst->lasttime;
	}
	(void) clock_gettime(CLOCK_MONOTONIC, &tv);
	return (inst->lasttime = (utimediff(&tv, &inst->systemreftime) + inst->systemtimeoffset));
}


/**
 * mb_player_pauseaudio() -- Pauses the audio stream and synchronizes
 * the audio clock.
 */
static int
mb_player_pauseaudio(struct mbp *inst)
{
	int err;
	uint64_t time;
	snd_pcm_status_t *status;
	snd_pcm_state_t state;
	snd_htimestamp_t audio_timestamp, audio_trigger_timestamp;
	snd_pcm_uframes_t avail;

	snd_pcm_status_alloca(&status);

	if ((err = snd_pcm_status(inst->audio_pcm_handle, status)) < 0) {
		printf("Stream status error: %s\n", snd_strerror(err));
		return -1;
	}

	state = snd_pcm_status_get_state(status);
	snd_pcm_status_get_trigger_htstamp(status, &audio_trigger_timestamp);
	snd_pcm_status_get_htstamp(status, &audio_timestamp);

	if (state == SND_PCM_STATE_RUNNING) {
		time  = ((audio_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_timestamp.tv_nsec) / 1000;
		time -= ((audio_trigger_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_trigger_timestamp.tv_nsec) / 1000;
		time += inst->audio_clock_offset;
	} else {
		time = inst->lasttime;
	}

	/* fprintf(stderr, "time=%lu offset=%li\n", time, inst->audio_clock_offset); */
	assert(time > 0 || inst->lasttime == 0);
	assert(time < INT64_MAX);

	/* set the clock to the current audio time */
	inst->audio_clock_offset = time;
	inst->audio_paused = 1;
	avail = snd_pcm_status_get_avail(status);

	/* move the clock up to what it should be after the ALSA buffer is drained
	 * and drain the buffer */
	inst->audio_clock_offset += ((1000 * 1000) / inst->audio_framerate) * (inst->audio_buffer_size - avail);
	snd_pcm_drain(inst->audio_pcm_handle);

	assert(inst->audio_clock_offset > 0);

	return 0;
}


/**
 * mb_player_resumeaudio() -- Resume audio playback
 */
static void
mb_player_resumeaudio(struct mbp *inst)
{
	/* if there's no frame ready we must wait for one before resuming */
	while (!inst->audio_quit && inst->audio_frame_state[inst->audio_playback_index] != 1) {
		pthread_mutex_lock(&inst->audio_lock);
		if (!inst->audio_quit && inst->audio_frame_state[inst->audio_playback_index] != 1) {
			pthread_cond_wait(&inst->audio_signal, &inst->audio_lock);
		}
		pthread_mutex_unlock(&inst->audio_lock);
	}

	if (inst->audio_quit) {
		return;
	}

	/* correct the audio clock to that of the next frame in the queue. */
	inst->audio_clock_offset = av_rescale_q(inst->audio_frame[inst->audio_playback_index]->pts,
		inst->audio_frame_timebase[inst->audio_playback_index], AV_TIME_BASE_Q);
	inst->audio_paused = 0;

	/* fprintf(stderr, "index=%i, state=%i, pts=%li, timebase=%i/%i\n",
		inst->audio_playback_index,
		inst->audio_frame_state[inst->audio_playback_index],
		inst->audio_frame[inst->audio_playback_index]->pts,
		inst->audio_frame_timebase[inst->audio_playback_index].num,
		inst->audio_frame_timebase[inst->audio_playback_index].den); */

	assert(inst->audio_clock_offset > 0);

	/* reset ALSA. The clock will start running again when the
	 * audio starts playing */
	snd_pcm_reset(inst->audio_pcm_handle);
	snd_pcm_prepare(inst->audio_pcm_handle);
	return;
}


/**
 * mb_player_adec_thread() -- This is the main decoding loop
 */
static void*
mb_player_audio(void *arg)
{
	int ret;
	struct mbp *inst = (struct mbp*) arg;
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
	assert(inst->audio_pcm_handle == NULL);
	assert(inst->audio_quit == 0);
	assert(inst->audio_paused == 0);

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_sw_params_alloca(&swparams);

	inst->audio_playback_running = 1;
	inst->audio_framerate = 48000;

	(void) mb_su_gainroot();

	/* initialize alsa device */
	if ((ret = snd_pcm_open(&inst->audio_pcm_handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		fprintf(stderr, "player: snd_pcm_open() failed, ret=%i\n",
			ret);
		goto audio_exit;
	}

	if ((ret = snd_pcm_hw_params_any(inst->audio_pcm_handle, params)) < 0) {
		fprintf(stderr, "player: Broken ALSA configuration: none available. %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_set_access(inst->audio_pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		fprintf(stderr, "player: INTERLEAVED RW access not available. %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_set_format(inst->audio_pcm_handle, params, SND_PCM_FORMAT_S16_LE)) < 0) {
		fprintf(stderr, "player: Format S16_LE not supported. %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_set_channels(inst->audio_pcm_handle, params, 2)) < 0) {
		fprintf(stderr, "player: 2 Channels not available. %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_hw_params_set_rate_near(inst->audio_pcm_handle, params, &inst->audio_framerate, &dir)) < 0) {
		fprintf(stderr, "player: 48000Hz not available. %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}
	#if 1
	if ((ret = snd_pcm_hw_params_set_period_size_near(inst->audio_pcm_handle, params, &period_frames, &dir)) < 0) {
		fprintf(stderr, "player: Cannot set period. %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}
	#else
	if ((ret = snd_pcm_hw_params_set_period_time_near(inst->audio_pcm_handle, params, &period_usecs, &dir)) < 0) {
		fprintf(stderr, "player: Could not set ALSA period time. %s\n",
			snd_strerror(ret));
		return NULL;
	}
	#endif
	if ((ret = snd_pcm_hw_params(inst->audio_pcm_handle, params)) < 0) {
		fprintf(stderr, "player: Could not set ALSA params: %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}

	if ((ret = snd_pcm_sw_params_current(inst->audio_pcm_handle, swparams)) < 0) {
		fprintf(stderr, "player: Could not determine SW params. %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_sw_params_set_tstamp_type(inst->audio_pcm_handle, swparams, SND_PCM_TSTAMP_TYPE_MONOTONIC)) < 0) {
		fprintf(stderr, "player: Could not set ALSA clock to CLOCK_MONOTONIC. %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}
	if ((ret = snd_pcm_sw_params(inst->audio_pcm_handle, swparams)) < 0) {
		fprintf(stderr, "player: Could not set ALSA SW paramms. %s\n",
			snd_strerror(ret));
		goto audio_exit;
	}

	if ((ret = snd_pcm_hw_params_get_period_time(params, &period_usecs, &dir)) < 0) {
		fprintf(stderr, "player: Could not get ALSA period time. %s\n",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_hw_params_get_rate(params, &inst->audio_framerate, &dir)) < 0) {
		fprintf(stderr, "player: Could not get ALSA framerate. %s\n",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_hw_params_get_period_size(params, &period_frames, &dir)) < 0) {
		fprintf(stderr, "player: Could not get ALSA period size. %s\n",
			snd_strerror(ret));
	}
	if ((ret = snd_pcm_hw_params_get_buffer_size(params, &inst->audio_buffer_size)) < 0) {
		fprintf(stderr, "player: Could not get ALSA buffer size. %s\n",
			snd_strerror(ret));
	}

	DEBUG_VPRINT("player", "ALSA buffer size: %lu", (unsigned long) inst->audio_buffer_size);
	DEBUG_VPRINT("player", "ALSA period size: %lu", (unsigned long) period_frames);
	DEBUG_VPRINT("player", "ALSA period time: %u", period_usecs);
	DEBUG_VPRINT("player", "ALSA framerate: %u", inst->audio_framerate);

	(void) mb_su_droproot();

	/* signal video thread that we're ready to start */
	pthread_mutex_lock(&inst->audio_lock);
	pthread_cond_broadcast(&inst->audio_signal);
	pthread_mutex_unlock(&inst->audio_lock);

	DEBUG_PRINT("player", "Audio thread ready");

	/* start decoding */
	while (LIKELY(inst->audio_quit == 0)) {
		/* pause */
		if (UNLIKELY(inst->audio_pause_requested)) {
			/* fprintf(stderr, "player: Audio pausing\n"); */
			mb_player_pauseaudio(inst);
			inst->audio_pause_requested = 0;
			while (!inst->audio_quit) {
				pthread_mutex_lock(&inst->resume_lock);
				pthread_cond_wait(&inst->resume_signal, &inst->resume_lock);
				pthread_mutex_unlock(&inst->resume_lock);
				if (inst->status == MB_PLAYER_STATUS_PAUSED) {
					continue;
				}
				mb_player_resumeaudio(inst);
				break;
			}
			/* fprintf(stderr, "player: Audio resuming\n"); */
		}

		/* if there's no frame ready we must wait */
		if (UNLIKELY(inst->audio_frame_state[inst->audio_playback_index] != 1)) {
			pthread_mutex_lock(&inst->audio_lock);
			if (inst->audio_quit) {
				pthread_mutex_unlock(&inst->audio_lock);
				continue;
			}
			if (LIKELY(inst->audio_frame_state[inst->audio_playback_index] != 1)) {
				pthread_mutex_unlock(&inst->audio_lock);
				/* fprintf(stderr, "player: Audio stalled\n"); */
				mb_player_pauseaudio(inst);
				mb_player_wait4buffers(inst, &inst->audio_quit);
				mb_player_resumeaudio(inst);
				/* fprintf(stderr, "player: Audio resuming\n"); */
				continue;
			}
			pthread_mutex_unlock(&inst->audio_lock);
			if (UNLIKELY(inst->audio_quit)) {
				continue;
			}
		}


		/* play the frame */
		frames = snd_pcm_writei(inst->audio_pcm_handle, inst->audio_frame[inst->audio_playback_index]->data[0],
			inst->audio_frame[inst->audio_playback_index]->nb_samples);
		if (UNLIKELY(frames < 0)) {
			fprintf(stderr, "player: underrun\n");

			/* frames = snd_pcm_recover(inst->audio_pcm_handle, frames, 0); */

			/* once underrun occurs getaudiotime() will continue to return
			 * the last time before underrun so we just call resumeaudio() to
			 * adjust the audio clock to that of the next frame and continue
			 * to write the next frame again */
			mb_player_resumeaudio(inst);
			continue;
		}
		if (UNLIKELY(frames < 0)) {
			fprintf(stderr, "player: snd_pcm_writei() failed: %s\n",
				snd_strerror(frames));
			av_frame_unref(inst->audio_frame[inst->audio_playback_index]);
			goto audio_exit;
		}


		/* free frame */
		av_frame_unref(inst->audio_frame[inst->audio_playback_index]);

		/* update buffer state and signal decoder */
		pthread_mutex_lock(&inst->audio_lock);
		inst->audio_frame_state[inst->audio_playback_index] = 0;
		pthread_cond_signal(&inst->audio_signal);
		pthread_cond_broadcast(&inst->audio_decoder_signal);
		pthread_mutex_unlock(&inst->audio_lock);

		inst->audio_playback_index++;
		inst->audio_playback_index %= MB_AUDIO_BUFFER_FRAMES;

		/* inst->audio_frames--; */
		__sync_fetch_and_sub(&inst->audio_frames, 1);

	}

audio_exit:
	DEBUG_PRINT("player", "Audio thread exiting");

	/* clear the have_audio flag and set the timer to system */
	inst->have_audio = 0;
	inst->audio_stream_index = -1;
	inst->audio_decoder_quit = 1;
	inst->getmastertime = mb_player_getsystemtime;
	pthread_cond_broadcast(&inst->audio_decoder_signal);

	/* cleanup */
	if (inst->audio_pcm_handle != NULL) {
		snd_pcm_close(inst->audio_pcm_handle);
		inst->audio_pcm_handle = NULL;
	}

	inst->audio_playback_running = 0;

	return NULL;
}


void *
mbv_dfb_getscreenmask();


static inline void
mb_player_fbdev_render(struct mbp *inst,
	int fd, struct fb_var_screeninfo *vinfo,
	struct fb_fix_screeninfo *finfo, void *fb_mem, void *buf)
{
	int x, y, pixelsz;
	unsigned int screen = 0;
	void *fb_buf;
	uint8_t *m = (uint8_t*) mbv_dfb_getscreenmask();

	pixelsz = vinfo->bits_per_pixel / CHAR_BIT;
	
#ifdef ENABLE_DOUBLE_BUFFERING
	fb_buf = inst->video_buffer;
#else
	fb_buf = fb_mem;
	(void) ioctl(fd, FBIO_WAITFORVSYNC, &screen);
#endif

	for (y = 0; y < vinfo->yres; y++) {
		for (x = 0; x < vinfo->xres; x++) {
			if (LIKELY(!m[(inst->width * y) + x])) {
				long location = (x + vinfo->xoffset) * pixelsz + (y + vinfo->yoffset) * finfo->line_length;
				uint32_t *ppix = (uint32_t*) buf;
				*((uint32_t*)(fb_buf + location)) = *(ppix + (((inst->width * y) + x)));
			}
		}
	}

#ifdef ENABLE_DOUBLE_BUFFERING
	(void) ioctl(fd, FBIO_WAITFORVSYNC, &screen);
	memcpy(fb_mem, inst->video_buffer, inst->bufsz * sizeof(uint8_t));
#endif
}


/**
 * mb_player_wait4audio() -- Waits for the audio stream to start playing
 */
static void
mb_player_wait4audio(struct mbp* inst, int *quit)
{
	int ret;
	snd_pcm_status_t *status;
	snd_pcm_state_t state;
	snd_pcm_status_alloca(&status);
	while (*quit == 0 && inst->audio_pcm_handle == NULL) {
		usleep(5000);
	}
	do {
		if ((ret = snd_pcm_status(inst->audio_pcm_handle, status)) < 0) {
			fprintf(stderr, "player: Could not get ALSA status\n");
			break;
		}
		usleep(1); /* do not raise this value */
		state = snd_pcm_status_get_state(status);
	}
	while (*quit == 0 && state != SND_PCM_STATE_RUNNING /* && state != SND_PCM_STATE_SETUP */);
}


static inline void
mb_player_postproc(struct mbp *inst, void *buf)
{
	/* if there is something to display in the top overlay
	 * then do it */
	if (UNLIKELY(inst->top_overlay_text != NULL)) {
		pthread_mutex_lock(&inst->top_overlay_lock);
		if (LIKELY(inst->top_overlay_text != NULL)) {
			/* create a cairo context for this frame */
			cairo_t *context;
			cairo_surface_t *surface;
			surface = cairo_image_surface_create_for_data(buf,
				CAIRO_FORMAT_ARGB32, inst->width, inst->height,
				cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, inst->width));
			if (LIKELY(surface != NULL)) {
				context = cairo_create(surface);
				cairo_surface_destroy(surface);

				if (LIKELY(context != NULL)) {
					PangoRectangle rect;
					rect.x = 15;
					rect.width = inst->width - 30;
					rect.y = 50;
					rect.height = 400;

					mb_player_rendertext(inst, context,
						inst->top_overlay_text, &rect);

					cairo_destroy(context);
				}
			}
		}
		pthread_mutex_unlock(&inst->top_overlay_lock);
	}
}

/**
 * mb_player_vrend_thread() -- Video rendering thread.
 */
#if (MB_VIDEO_BUFFER_FRAMES > 1)
static void *
mb_player_video(void *arg)
{
	uint8_t *buf;
	int64_t frame_pts, delay, frame_time = 0;
	struct mbp *inst = (struct mbp*) arg;
	
#ifdef MB_DECODER_PRINT_FPS
	struct timespec new_tp, last_tp, elapsed_tp;
	int frames = 0, fps = 0;
#endif
#ifdef MB_FBDEV_RENDERER
	int fd = -1;
	long screensize;
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	void *fb_mem = NULL;
#endif

	MB_DEBUG_SET_THREAD_NAME("video_playback");
	DEBUG_PRINT("player", "Video renderer started");

	assert(inst != NULL);
	assert(inst->video_quit == 0);

	inst->video_playback_running = 1;

	/* initialize framebuffer for direct rendering */
	if (inst->use_fbdev) {
		mb_su_gainroot();
		if ((fd = open("/dev/fb0", O_RDWR)) != -1) {
			if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == -1 ||
				ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1) {
				fprintf(stderr, "player: ioctl() failed. "
					"Direct rendering disabled\n");
				inst->use_fbdev = 0;
				close(fd);
			} else {
				screensize = vinfo.yres_virtual * finfo.line_length;
				fb_mem = mmap(0, screensize, PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, (off_t) 0);
				if (fb_mem == MAP_FAILED) {
					fprintf(stderr, "player: mmap() failed. "
						"Direct rendering disabled\n");
					inst->use_fbdev = 0;
					close(fd);
				}
			}
		} else {
			inst->use_fbdev = 0;
		}
		mb_su_droproot();
	}

	DEBUG_PRINT("player", "Video renderer ready");

	/* signal control thread that we're ready */
	pthread_mutex_lock(&inst->video_output_lock);
	pthread_cond_broadcast(&inst->video_output_signal);
	pthread_mutex_unlock(&inst->video_output_lock);


#ifdef MB_DECODER_PRINT_FPS
	(void) clock_gettime(CLOCK_MONOTONIC, &last_tp);
	new_tp = last_tp;
#endif

	if (inst->have_audio) {
		/* wait for the audio to start playing */
		mb_player_wait4audio(inst, &inst->video_quit);
	} else {
		/* save the reference timestamp */
		mb_player_wait4buffers(inst, &inst->video_quit);
		mb_player_resetsystemtime(inst, 0);
	}

	while (LIKELY(!inst->video_quit)) {
		/* if there's no frame ready we must wait */
		if (UNLIKELY(inst->frame_state[inst->video_playback_index] != 1)) {
			pthread_mutex_lock(&inst->video_output_lock);
			if (inst->video_quit) {
				pthread_mutex_unlock(&inst->video_output_lock);
				goto video_exit;
			}
			if (LIKELY(inst->frame_state[inst->video_playback_index] != 1)) {
				/* fprintf(stderr, "player: Waiting for video decoder\n"); */
				if (inst->have_audio) {
					/* fprintf(stderr, "player: Video stalled\n"); */
					inst->audio_pause_requested = 1;
					pthread_mutex_unlock(&inst->video_output_lock);
					while (!inst->audio_quit && inst->audio_pause_requested == 1) {
						pthread_cond_broadcast(&inst->audio_signal);
						usleep(1000);
					}
					mb_player_wait4buffers(inst, &inst->video_quit);
					while (!inst->video_quit && inst->audio_paused) {
						pthread_cond_broadcast(&inst->resume_signal);
						usleep(1000);
					}
					/* fprintf(stderr, "player: Video resuming\n"); */
				} else {
					pthread_cond_wait(&inst->video_output_signal, &inst->video_output_lock);
					pthread_mutex_unlock(&inst->video_output_lock);
					mb_player_wait4buffers(inst, &inst->video_quit);
					mb_player_resetsystemtime(inst, frame_time);
				}
				continue;
			}
			pthread_mutex_unlock(&inst->video_output_lock);
		}

		/* dereference the frame pointer for later use */
		buf = inst->frame_data[inst->video_playback_index];

		/* copy to the last frame buffer in case we need to
		 * repaint the screen. This is fast so it makes sense
		 * to do it for every frame even if it will get skipped
		 * because that way if the video is paused the latest
		 * frame can be drawn even if we're skipping frames */
		memcpy(inst->video_last_frame, buf, inst->bufsz);

		/* perform post processing (overlays, etc) */
		mb_player_postproc(inst, buf);

		/* get the frame pts */
		frame_pts = inst->frame_pts[inst->video_playback_index];

		if  (LIKELY(frame_pts != AV_NOPTS_VALUE)) {
			int64_t elapsed;

			frame_time = av_rescale_q(frame_pts,
				inst->frame_time_base[inst->video_playback_index], AV_TIME_BASE_Q);

recalc:
			if (UNLIKELY(inst->video_quit)) {
				continue;
			}

			elapsed = inst->getmastertime(inst);
			if (UNLIKELY(elapsed > frame_time)) {
				delay = 0;
				if (elapsed - frame_time > 100000) {
					/* fprintf(stderr, "player: skipping frame_time=%li elapsed=%li diff=%li\n",
						frame_time, elapsed, elapsed - frame_time); */

					/* if the decoder is lagging behind tell it to
					 * skip a few frames */
					if (UNLIKELY(mb_player_dumpvideo(inst, 0))) {
						continue;
					}

					/* skip frame */
					goto frame_complete;
				}
			} else {
				delay = frame_time - elapsed;
			}

			delay &= ~0xFF;	/* a small delay  will only waste time (context switch
					   time + recalc time) and we'll loose time because we're not
					   likely to wake up that quick */

			if (LIKELY(delay > 0)) {
				/* if we're paused then sleep a good while */
				if (inst->have_audio) {
					/**
					 * So we're waiting for the clock but the audio thread is waiting
					 * for more frames (so the clock will never advance). It looks like
					 * the video fell behing and the audio stream dryed out, but why
					 * is the audio clock behind the last video frame??
					 */
					if (inst->audio_paused && inst->audio_packets == 0 && inst->audio_frames == 0) {
						mb_player_dumpvideo(inst, 1);
						fprintf(stderr, "Deadlock detected, recovered (I hope)\n");
					}
				} else {
					if (UNLIKELY(inst->video_paused)) {
						usleep(1000 * 500);
					}
				}

				usleep(delay);

				/* the clock may have been stopped while we slept */
				goto recalc;

			}
		}


		if (LIKELY(inst->use_fbdev)) {
			mb_player_fbdev_render(inst, fd, &vinfo, &finfo, fb_mem, buf);
		} else  {
			/* blit the frame through window manager */
			mbv_window_blit_buffer(inst->window, buf, inst->width, inst->height, 0, 0);
		}

#ifdef MB_DECODER_PRINT_FPS
		/* calculate fps */
		frames++;
		(void) clock_gettime(CLOCK_MONOTONIC, &new_tp);
		elapsed_tp = timediff(&last_tp, &new_tp);
		if (UNLIKELY(elapsed_tp.tv_sec > 0)) {
			(void) clock_gettime(CLOCK_MONOTONIC, &last_tp);
			fps = frames;
			frames = 0;
		}
		mb_player_printstatus(inst, fps);
#endif

frame_complete:
		/* update buffer state and signal decoder */
		pthread_mutex_lock(&inst->video_output_lock);
		inst->frame_state[inst->video_playback_index] = 0;
		/* inst->video_frames--; */
		__sync_fetch_and_sub(&inst->video_frames, 1);
		pthread_cond_signal(&inst->video_output_signal);
		pthread_cond_broadcast(&inst->video_decoder_signal);
		pthread_mutex_unlock(&inst->video_output_lock);

		inst->video_playback_index++;
		inst->video_playback_index %= MB_VIDEO_BUFFER_FRAMES;
	}

video_exit:
	DEBUG_PRINT("player", "Video renderer exiting");

	/* clear screen */
	memset(inst->frame_data[0], 0, inst->bufsz);
	if (inst->use_fbdev) {
		mb_player_fbdev_render(inst, fd, &vinfo, &finfo, fb_mem, inst->frame_data[0]);
	} else {
		mbv_window_blit_buffer(inst->window, inst->frame_data[0], inst->width, inst->height, 0, 0);
	}

	/* denitialize fbdev */
	if (inst->use_fbdev) {
		close(fd);
	}

	inst->video_playback_running = 0;

	return NULL;
	
}
#endif


/**
 * mb_player_update() -- Update the player window
 */
void
mb_player_update(struct mbp *inst)
{
	void *frame_data;

	DEBUG_PRINT("player", "Updating surface");

	assert(inst != NULL);

	/* if the last frame buffer is NULL it means that we're not
	 * currently playing
	 *
	 * FIXME: There is a race condition here. If the last frame
	 * if freed after this check we may crash or at least get a video
	 * glitch. Probably not worth fixing as once the DRM compositor
	 * is working and the overlays move to the shell it won't be
	 * necessary to cache the last frame here.
	 */
	if (inst->video_last_frame == NULL) {
		return;
	}

	if ((frame_data = av_malloc(inst->bufsz)) == NULL) {
		return;
	}

	memcpy(frame_data, inst->video_last_frame, inst->bufsz);
	mb_player_postproc(inst, frame_data);
	mbv_window_blit_buffer(inst->window, frame_data, inst->width, inst->height, 0, 0);
	free(frame_data);
}


/**
 * mb_player_initfilters() -- Initialize ffmpeg's filter graph
 */
static int
mb_player_initvideofilters(
	AVFormatContext *fmt_ctx,
	AVCodecContext *dec_ctx,
	AVFilterContext **buffersink_ctx,
	AVFilterContext **buffersrc_ctx,
	AVFilterGraph **filter_graph,
	const char *filters_descr,
	int stream_index)
{
	char args[512];
	int ret = 0;
	AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	AVFilter *buffersink = avfilter_get_by_name("buffersink");

	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	AVRational time_base = fmt_ctx->streams[stream_index]->time_base;
	enum AVPixelFormat pix_fmts[] = { MB_DECODER_PIX_FMT, AV_PIX_FMT_NONE };

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
		time_base.num, time_base.den,
		dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);
	fprintf(stderr, "args: %s\n", args);


	*filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !*filter_graph) {
		ret = AVERROR(ENOMEM);
		goto end;
	}

	ret = avfilter_graph_create_filter(buffersrc_ctx, buffersrc, "in",
                                       args, NULL, *filter_graph);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
		goto end;
	}

	/* buffer video sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(buffersink_ctx, buffersink, "out",
                                       NULL, NULL, *filter_graph);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
		goto end;
	}

	ret = av_opt_set_int_list(*buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
		goto end;
	}

	/*
	 * Set the endpoints for the filter graph. The filter_graph will
	 * be linked to the graph described by filters_descr.
	 */

	/*
	 * The buffer source output must be connected to the input pad of
	 * the first filter described by filters_descr; since the first
	 * filter input label is not specified, it is set to "in" by
	 * default.
	 */
	outputs->name       = av_strdup("in");
	outputs->filter_ctx = *buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	/*
	 * The buffer sink input must be connected to the output pad of
	 * the last filter described by filters_descr; since the last
	 * filter output label is not specified, it is set to "out" by
	 * default.
	 */
	inputs->name       = av_strdup("out");
	inputs->filter_ctx = *buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	if ((ret = avfilter_graph_parse_ptr(*filter_graph, filters_descr,
                                    &inputs, &outputs, NULL)) < 0) {
		goto end;
	}

	if ((ret = avfilter_graph_config(*filter_graph, NULL)) < 0) {
	        goto end;
	}


end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}


static int
mb_player_initaudiofilters(
	AVFormatContext *fmt_ctx,
	AVCodecContext *dec_ctx,
	AVFilterContext **buffersink_ctx,
	AVFilterContext **buffersrc_ctx,
	AVFilterGraph **filter_graph,
	const char *filters_descr,
	int audio_stream_index)
{
    char args[512];
    int ret = 0;
    AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    static const enum AVSampleFormat out_sample_fmts[] = { AV_SAMPLE_FMT_S16, -1 };
    static const int64_t out_channel_layouts[] = { AV_CH_LAYOUT_STEREO, -1 };
    static const int out_sample_rates[] = { 48000, -1 };
    const AVFilterLink *outlink;
    AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;

    *filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !*filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer audio source: the decoded frames from the decoder will be inserted here. */
    if (!dec_ctx->channel_layout)
        dec_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);
    snprintf(args, sizeof(args),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
             time_base.num, time_base.den, dec_ctx->sample_rate,
             av_get_sample_fmt_name(dec_ctx->sample_fmt), dec_ctx->channel_layout);
    ret = avfilter_graph_create_filter(buffersrc_ctx, abuffersrc, "in",
                                       args, NULL, *filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
        goto end;
    }

    /* buffer audio sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(buffersink_ctx, abuffersink, "out",
                                       NULL, NULL, *filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
        goto end;
    }

    ret = av_opt_set_int_list(*buffersink_ctx, "sample_fmts", out_sample_fmts, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
        goto end;
    }

    ret = av_opt_set_int_list(*buffersink_ctx, "channel_layouts", out_channel_layouts, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
        goto end;
    }

    ret = av_opt_set_int_list(*buffersink_ctx, "sample_rates", out_sample_rates, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */
    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = *buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = *buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(*filter_graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(*filter_graph, NULL)) < 0)
        goto end;

    /* Print summary of the sink buffer
     * Note: args buffer is reused to store channel layout string */
    outlink = (*buffersink_ctx)->inputs[0];
    av_get_channel_layout_string(args, sizeof(args), -1, outlink->channel_layout);
    av_log(NULL, AV_LOG_INFO, "Output: srate:%dHz fmt:%s chlayout:%s\n",
           (int)outlink->sample_rate,
           (char *)av_x_if_null(av_get_sample_fmt_name(outlink->format), "?"),
           args);

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}


static int
open_codec_context(int *stream_idx,
                              AVFormatContext *fmt_ctx, enum AVMediaType type)
{
	int ret;
	AVStream *st;
	AVCodecContext *dec_ctx = NULL;
	AVCodec *dec = NULL;
	AVDictionary *opts = NULL;

	ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
	if (ret < 0) {
		fprintf(stderr, "Could not find %s stream in input file\n",
		av_get_media_type_string(type));
		return ret;
	} else {
		*stream_idx = ret;
		st = fmt_ctx->streams[*stream_idx];

		/* find decoder for the stream */
		dec_ctx = st->codec;
		dec = avcodec_find_decoder(dec_ctx->codec_id);
		if (!dec) {
			fprintf(stderr, "Failed to find %s codec\n",
				av_get_media_type_string(type));
			return AVERROR(EINVAL);
		}

		/* Init the video decoder */
		av_dict_set(&opts, "flags2", "+export_mvs", 0);
		if ((ret = avcodec_open2(dec_ctx, dec, &opts)) < 0) {
			fprintf(stderr, "Failed to open %s codec\n",
				av_get_media_type_string(type));
			return ret;
		}
	}
	return 0;
}


/**
 * mb_player_video_decode() -- Decodes video frames in the background.
 */
static void *
mb_player_video_decode(void *arg)
{
	int i, finished;
	struct mbp *inst = (struct mbp*) arg;
	char video_filters[512];
	AVFrame *video_frame_nat = NULL, *video_frame_flt = NULL;
	AVFilterGraph *video_filter_graph = NULL;
	AVFilterContext *video_buffersink_ctx = NULL;
	AVFilterContext *video_buffersrc_ctx = NULL;

	MB_DEBUG_SET_THREAD_NAME("video_decode");
	DEBUG_PRINT("player", "Video decoder starting");

	assert(inst != NULL);
	assert(inst->fmt_ctx != NULL);
	assert(inst->video_stream_index == -1);
	assert(inst->video_decoder_pts == 0);
	assert(inst->video_codec_ctx == NULL);

	/* initialize all frame data buffers to NULL */
	inst->video_last_frame = NULL;
#ifdef ENABLE_DOUBLE_BUFFERING
	inst->video_buffer = NULL;
#endif
	for (i = 0; i < MB_VIDEO_BUFFER_FRAMES; i++) {
		inst->frame_data[i] = NULL;
	}

	/* open the video codec */
	if (open_codec_context(&inst->video_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
		inst->video_codec_ctx = inst->fmt_ctx->streams[inst->video_stream_index]->codec;
	}
	if (inst->video_codec_ctx == NULL) {
		goto decoder_exit;
	}

	/* initialize video filter graph */
	snprintf(video_filters, sizeof(video_filters),
		"scale='if(gt(a,4/3),%i,-1)':'if(gt(a,4/3),-1,%i)',"
		"pad=%i:%i:'((out_w - in_w) / 2)':'((out_h - in_h) / 2)'",
		inst->width, inst->height, inst->width, inst->height);

	DEBUG_VPRINT("player", "Video filters: %s", video_filters);

	if (mb_player_initvideofilters(inst->fmt_ctx, inst->video_codec_ctx,
		&video_buffersink_ctx, &video_buffersrc_ctx, &video_filter_graph,
		video_filters, inst->video_stream_index) < 0) {
		fprintf(stderr, "player: Could not init filter graph!\n");
		goto decoder_exit;
	}

	/* calculate the size of each frame and allocate buffer for it */
	inst->bufsz = avpicture_get_size(MB_DECODER_PIX_FMT, inst->width, inst->height);
#ifdef ENABLE_DOUBLE_BUFFERING
	inst->video_buffer = av_malloc(inst->bufsz * sizeof(int8_t));
	if (inst->video_buffer == NULL) {
		goto decoder_exit;
	}
#endif
	inst->video_last_frame = av_malloc(inst->bufsz * sizeof(int8_t));
	if (inst->video_last_frame == NULL) {
		goto decoder_exit;
	}
	for (i = 0; i < MB_VIDEO_BUFFER_FRAMES; i++) {
		inst->frame_data[i] = av_malloc(inst->bufsz * sizeof(int8_t));
		if (inst->frame_data[i] == NULL) {
			goto decoder_exit;
		}
		inst->frame_state[i] = 0;
	}

	DEBUG_VPRINT("player", "video_codec_ctx: width=%i height=%i pix_fmt=%i",
		inst->width, inst->height, inst->video_codec_ctx->pix_fmt);

	/* allocate video frames */
	video_frame_nat = av_frame_alloc(); /* native */
	video_frame_flt = av_frame_alloc(); /* filtered */
	if (video_frame_nat == NULL || video_frame_flt == NULL) {
		fprintf(stderr, "player: Could not allocate frames\n");
		goto decoder_exit;
	}

	DEBUG_PRINT("player", "Video decoder ready");

	/* signal control trhead that we're ready */
	pthread_mutex_lock(&inst->video_decoder_lock);
	pthread_cond_signal(&inst->video_decoder_signal);
	pthread_mutex_unlock(&inst->video_decoder_lock);


	while (LIKELY(!inst->video_decoder_quit)) {
		/* if there's no frame to decode wait */
		if (UNLIKELY(inst->video_packet_state[inst->video_packet_read_index] != 1)) {
			pthread_mutex_lock(&inst->video_decoder_lock);
			if (UNLIKELY(inst->video_decoder_quit)) {
				pthread_mutex_unlock(&inst->video_decoder_lock);
				goto decoder_exit;
			}
			if (LIKELY(inst->video_packet_state[inst->video_packet_read_index] != 1)) {
				pthread_cond_wait(&inst->video_decoder_signal, &inst->video_decoder_lock);
				pthread_mutex_unlock(&inst->video_decoder_lock);
				continue;
			}
			pthread_mutex_unlock(&inst->video_decoder_lock);
			if (UNLIKELY(inst->video_decoder_quit)) {
				continue;
			}
		}

		/* decode frame */
		if (UNLIKELY((i = avcodec_decode_video2(inst->video_codec_ctx, video_frame_nat, &finished, &inst->video_packet[inst->video_packet_read_index])) < 0)) {
			fprintf(stderr, "player: avcodec_decode_video2() returned %i\n", i);
		}

		if (LIKELY(finished)) {
			int64_t frame_pts = video_frame_nat->pts =
				av_frame_get_best_effort_timestamp(video_frame_nat);

			/* push the decoded frame into the filtergraph */
			if (UNLIKELY(av_buffersrc_add_frame_flags(video_buffersrc_ctx,
				video_frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)) {
				fprintf(stderr, "player: Error feeding filterchain\n");
				goto decoder_exit;
			}

			/* pull filtered frames from the filtergraph */
			while (!inst->video_decoder_quit) {
				i = av_buffersink_get_frame(video_buffersink_ctx, video_frame_flt);
				if (UNLIKELY(i == AVERROR(EAGAIN) || i == AVERROR_EOF)) {
					break;
				}
				if (UNLIKELY(i < 0)) {
					goto decoder_exit;
				}

				/* if the renderer has not finished we must wait */
				while (UNLIKELY(inst->frame_state[inst->video_decode_index] != 0)) {
					pthread_mutex_lock(&inst->video_output_lock);
					if (UNLIKELY(inst->video_decoder_quit)) {
						pthread_mutex_unlock(&inst->video_output_lock);
						goto decoder_exit;
					}
					if (LIKELY(inst->frame_state[inst->video_decode_index] != 0)) {
						/*fprintf(stderr, "player: "
							"Waiting for renderer\n"); */
						pthread_cond_wait(&inst->video_output_signal,
							&inst->video_output_lock);
					}
					pthread_mutex_unlock(&inst->video_output_lock);
					if (UNLIKELY(inst->video_decoder_quit)) {
						goto decoder_exit;
					}
				}

				/* copy picture to buffer */
				avpicture_layout((const AVPicture*) video_frame_flt,
					MB_DECODER_PIX_FMT, inst->width, inst->height,
					inst->frame_data[inst->video_decode_index], inst->bufsz);

				inst->frame_repeat[inst->video_decode_index] = video_frame_flt->repeat_pict;
				inst->frame_pts[inst->video_decode_index] = frame_pts;
				inst->frame_time_base[inst->video_decode_index] = 
					video_buffersink_ctx->inputs[0]->time_base;

				/* update the buffer index and signal renderer thread */
				pthread_mutex_lock(&inst->video_output_lock);
				inst->video_decoder_pts = frame_pts;
				inst->video_decoder_timebase = video_buffersink_ctx->inputs[0]->time_base;
				inst->frame_state[inst->video_decode_index] = 1;
				/* inst->video_frames++; */
				__sync_fetch_and_add(&inst->video_frames, 1);
				pthread_cond_signal(&inst->video_output_signal);
				pthread_mutex_unlock(&inst->video_output_lock);

				inst->video_decode_index++;
				inst->video_decode_index %= MB_VIDEO_BUFFER_FRAMES;

				av_frame_unref(video_frame_flt);
			}
			av_frame_unref(video_frame_nat);
		}
		/* free packet */
		av_free_packet(&inst->video_packet[inst->video_packet_read_index]);
		pthread_mutex_lock(&inst->video_decoder_lock);
		inst->video_packet_state[inst->video_packet_read_index] = 0;
		inst->video_packet_read_index++;
		inst->video_packet_read_index %= MB_VIDEO_BUFFER_PACKETS;
		pthread_cond_signal(&inst->video_decoder_signal);
		pthread_mutex_unlock(&inst->video_decoder_lock);

		/* inst->video_packets--; */
		__sync_fetch_and_sub(&inst->video_packets, 1);
	}
decoder_exit:
	DEBUG_PRINT("player", "Video decoder exiting");

	if (inst->video_last_frame != NULL) {
		free(inst->video_last_frame);
		inst->video_last_frame = NULL;
	}

#ifdef ENABLE_DOUBLE_BUFFERING
	if (inst->video_buffer != NULL) {
		free(inst->video_buffer);
		inst->video_buffer = NULL;
	}
#endif

	for (i = 0; i < MB_VIDEO_BUFFER_FRAMES; i++) {
		if (inst->frame_data[i] != NULL) {
			free(inst->frame_data[i]);
			inst->frame_data[i] = NULL;
		}
	}
	if (video_frame_nat != NULL) {
		av_free(video_frame_nat);
	}
	if (video_frame_flt != NULL) {
		av_free(video_frame_flt);
	}
	if (video_buffersink_ctx != NULL) {
		avfilter_free(video_buffersink_ctx);
	}
	if (video_buffersrc_ctx != NULL) {
		avfilter_free(video_buffersrc_ctx);
	}
	if (video_filter_graph != NULL) {
		avfilter_graph_free(&video_filter_graph);
	}
	if (inst->video_codec_ctx != NULL) {
		avcodec_close(inst->video_codec_ctx);
		inst->video_codec_ctx = NULL;
	}

	return NULL;
}


/**
 * mb_player_audio_decode() -- Decodes the audio stream.
 */
static void *
mb_player_audio_decode(void * arg)
{
	int finished = 0, ret;
	struct mbp* inst = (struct mbp*) arg;
	const char *audio_filters ="aresample=48000,aformat=sample_fmts=s16:channel_layouts=stereo";
	AVCodecContext *audio_codec_ctx = NULL;
	AVFrame *audio_frame_nat = NULL;
	AVFilterGraph *audio_filter_graph = NULL;
	AVFilterContext *audio_buffersink_ctx = NULL;
	AVFilterContext *audio_buffersrc_ctx = NULL;
	AVPacket packet, packet1;

	MB_DEBUG_SET_THREAD_NAME("audio_decoder");


	assert(inst != NULL);
	assert(inst->audio_decoder_quit == 0);
	assert(inst->fmt_ctx != NULL);
	assert(inst->audio_stream_index == -1);

	DEBUG_PRINT("player", "Audio decoder starting");

	/* open the audio codec */
	if (open_codec_context(&inst->audio_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
		audio_codec_ctx = inst->fmt_ctx->streams[inst->audio_stream_index]->codec;	
	}
	if (audio_codec_ctx == NULL) {
		goto decoder_exit;
	}

	/* allocate audio frames */
	audio_frame_nat = av_frame_alloc(); /* native */
	if (audio_frame_nat == NULL) {
		fprintf(stderr, "player: Could not allocate audio frames\n");
		goto decoder_exit;
	}

	/* initialize audio filter graph */
	fprintf(stderr, "player: audio_filters: %s\n",
		audio_filters);
	if (mb_player_initaudiofilters(inst->fmt_ctx, audio_codec_ctx,
		&audio_buffersink_ctx, &audio_buffersrc_ctx, &audio_filter_graph,
		audio_filters, inst->audio_stream_index) < 0) {
		fprintf(stderr, "player: Could not init filter graph!\n");
		goto decoder_exit;
	}

	DEBUG_PRINT("player", "Audio decoder ready");

	/* signl control thread that we're ready */
	pthread_mutex_lock(&inst->audio_decoder_lock);
	pthread_cond_signal(&inst->audio_decoder_signal);
	pthread_mutex_unlock(&inst->audio_decoder_lock);


	while (LIKELY(!inst->audio_decoder_quit)) {
		/* wait for the stream decoder to give us some packets */
		if (UNLIKELY(inst->audio_packet_state[inst->audio_packet_read_index] != 1)) {
			pthread_mutex_lock(&inst->audio_decoder_lock);
			if (UNLIKELY(inst->audio_decoder_quit)) {
				pthread_mutex_unlock(&inst->audio_decoder_lock);
				continue;
			}
			if  (LIKELY(inst->audio_packet_state[inst->audio_packet_read_index] != 1)) {
				pthread_cond_wait(&inst->audio_decoder_signal, &inst->audio_decoder_lock);
				pthread_mutex_unlock(&inst->audio_decoder_lock);
				continue;
			}
		}

		/* grab the packet and signal the stream decoder thread */
		packet = packet1 = inst->audio_packet[inst->audio_packet_read_index];
		inst->audio_packet_state[inst->audio_packet_read_index] = 0;
		pthread_cond_signal(&inst->audio_decoder_signal);
		pthread_mutex_unlock(&inst->audio_decoder_lock);

		/*inst->audio_packets--;*/
		__sync_fetch_and_sub(&inst->audio_packets, 1);

		inst->audio_packet_read_index++;
		inst->audio_packet_read_index %= MB_AUDIO_BUFFER_PACKETS;

		/* decode audio frame */
		while (LIKELY(packet1.size > 0)) {
			finished = 0;
			ret = avcodec_decode_audio4(audio_codec_ctx, audio_frame_nat,
				&finished, &packet1);
			if (UNLIKELY(ret < 0)) {
				av_log(NULL, AV_LOG_ERROR, "Error decoding audio\n");
				continue;
			}
			packet1.size -= ret;
			packet1.data += ret;

			if (LIKELY(finished)) {
				audio_frame_nat->pts =
					av_frame_get_best_effort_timestamp(audio_frame_nat);

				/* push the audio data from decoded frame into the filtergraph */
				if (UNLIKELY(av_buffersrc_add_frame_flags(audio_buffersrc_ctx,
					audio_frame_nat, 0) < 0)) {
					av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph\n");
					break;
				}

				/* pull filtered audio from the filtergraph */
				while (LIKELY(!inst->audio_decoder_quit)) {

					/* if the video output has not finished we must wait */
					if (UNLIKELY(inst->audio_frame_state[inst->audio_decode_index] != 0)) {
						pthread_mutex_lock(&inst->audio_lock);
						if (UNLIKELY(inst->audio_decoder_quit)) {
							pthread_mutex_unlock(&inst->audio_lock);
							continue;
						}
						if (LIKELY(inst->audio_frame_state[inst->audio_decode_index] != 0)) {
							/*fprintf(stderr, "player: "
								"Decoder waiting for audio thread\n"); */
							pthread_cond_wait(&inst->audio_signal,
								&inst->audio_lock);
						}
						pthread_mutex_unlock(&inst->audio_lock);
						continue;
					}

					ret = av_buffersink_get_frame(audio_buffersink_ctx,
						inst->audio_frame[inst->audio_decode_index]);
					if (UNLIKELY(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)) {
						av_frame_unref(inst->audio_frame[inst->audio_decode_index]);
						break;
					}
					if (UNLIKELY(ret < 0)) {
						av_frame_unref(inst->audio_frame[inst->audio_decode_index]);
						goto decoder_exit;
					}

					inst->audio_frame_timebase[inst->audio_decode_index] = 
						audio_buffersink_ctx->inputs[0]->time_base;

					/* update the buffer index and signal renderer thread */
					pthread_mutex_lock(&inst->audio_lock);
					inst->audio_frame_state[inst->audio_decode_index] = 1;
					pthread_cond_signal(&inst->audio_signal);
					pthread_mutex_unlock(&inst->audio_lock);

					inst->audio_decode_index++;
					inst->audio_decode_index %= MB_AUDIO_BUFFER_FRAMES;

					/* inst->audio_frames++; */
					__sync_fetch_and_add(&inst->audio_frames, 1);
				}
			}
		}
		/* free packet */
		av_free_packet(&packet);
	}

decoder_exit:
	DEBUG_PRINT("player", "Audio decoder exiting");

	if (audio_frame_nat != NULL) {
		av_free(audio_frame_nat);
	}
	if (audio_buffersink_ctx != NULL) {
		avfilter_free(audio_buffersink_ctx);
	}
	if (audio_buffersink_ctx != NULL) {
		avfilter_free(audio_buffersrc_ctx);
	}
	if (audio_filter_graph != NULL) {
		avfilter_graph_free(&audio_filter_graph);
	}
	if (audio_codec_ctx != NULL) {
		avcodec_close(audio_codec_ctx);
	}

	return NULL;
}


/**
 * mb_player_stream_decode() -- This is the main decoding loop
 */
static void*
mb_player_stream_decode(void *arg)
{
	struct mbp *inst = (struct mbp*) arg;
	int i;
	AVPacket packet;


	MB_DEBUG_SET_THREAD_NAME("stream_input");


	assert(inst != NULL);
	assert(inst->media_file != NULL);
	assert(inst->window != NULL);
	assert(inst->status == MB_PLAYER_STATUS_PLAYING || inst->status == MB_PLAYER_STATUS_BUFFERING);
	assert(inst->fmt_ctx == NULL);

	inst->have_audio = 0;
	inst->have_video = 0;
	inst->audio_paused = 0;
	inst->video_paused = 0;
	inst->audio_stream_index = -1;
	inst->video_stream_index = -1;
	inst->audio_packets = 0;
	inst->audio_frames = 0;
	inst->video_frames = 0;
	inst->video_packets = 0;
	inst->lasttime = 0;
	inst->seek_to = -1;

	/* get the size of the window */
	if (mbv_window_getsize(inst->window, &inst->width, &inst->height) == -1) {
		fprintf(stderr, "player: Could not get window size\n");
		goto decoder_exit;
	}

	DEBUG_VPRINT("player", "Attempting to play (%ix%i) '%s'",
		inst->width, inst->height, inst->media_file);

	/* open file */
	if (avformat_open_input(&inst->fmt_ctx, inst->media_file, NULL, NULL) != 0) {
		fprintf(stderr, "player: Could not open '%s'\n",
			inst->media_file);
		goto decoder_exit;
	}

	if (avformat_find_stream_info(inst->fmt_ctx, NULL) < 0) {
		fprintf(stderr, "player: Could not find stream info\n");
		goto decoder_exit;
	}

	/* dump file info */
	av_dump_format(inst->fmt_ctx, 0, inst->media_file, 0);

	/* if the file contains a video stream fire the video decoder */
	if (av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0) >= 0) {

		inst->video_quit = 0;
		inst->video_decoder_quit = 0;
		inst->video_playback_index = 0;
		inst->video_decode_index = 0;
		inst->video_packet_read_index = 0;
		inst->video_packet_write_index = 0;
		inst->video_skipframes = 0;
		inst->video_decoder_pts = 0;
		inst->getmastertime = mb_player_getsystemtime;
		inst->have_video = 1;

		/* initialize all packet states */
		for (i = 0; i < MB_VIDEO_BUFFER_PACKETS; i++) {
			inst->video_packet_state[i] = 0;
		}
		/* fire the video decoder thread */
		pthread_mutex_lock(&inst->video_decoder_lock);
		if (pthread_create(&inst->video_decoder_thread, NULL, mb_player_video_decode, inst) != 0) {
			abort();
		}
		pthread_cond_wait(&inst->video_decoder_signal, &inst->video_decoder_lock);
		pthread_mutex_unlock(&inst->video_decoder_lock);

		fprintf(stderr, "player: Video stream: %i\n", inst->video_stream_index);
	}

	/* if there's an audio stream start the audio decoder */
	if (av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) >= 0) {

		DEBUG_PRINT("player", "Audio stream found");

		/* allocate filtered audio frames */
		inst->audio_pcm_handle = NULL;
		inst->audio_decode_index = 0;
		inst->audio_playback_index = 0;
		inst->audio_quit = 0;
		inst->audio_decoder_quit = 0;
		inst->audio_pause_requested = 0;
		inst->audio_stream_index = -1;
		inst->audio_packet_write_index = 0;
		inst->audio_packet_read_index = 0;
		inst->audio_clock_offset = 0;
		inst->getmastertime = mb_player_getaudiotime; /* video is slave to audio */
		inst->have_audio = 1;

		for (i = 0; i < MB_AUDIO_BUFFER_FRAMES; i++) {
			inst->audio_frame[i] = av_frame_alloc();
			inst->audio_frame_state[i] = 0;
			assert(inst->audio_frame[i] != NULL);
		}
		for (i = 0; i < MB_AUDIO_BUFFER_PACKETS; i++) {
			inst->audio_packet_state[i] = 0;
		}

		/* start the audio decoder thread and wait until it's ready to decode */
		pthread_mutex_lock(&inst->audio_decoder_lock);
		if (pthread_create(&inst->audio_decoder_thread, NULL, mb_player_audio_decode, inst) != 0) {
			abort();
		}
		pthread_cond_wait(&inst->audio_decoder_signal, &inst->audio_decoder_lock);
		pthread_mutex_unlock(&inst->audio_decoder_lock);
	}

	DEBUG_PRINT("player", "Stream decoder ready");

	pthread_mutex_lock(&inst->resume_lock);
	pthread_cond_signal(&inst->resume_signal);
	pthread_mutex_unlock(&inst->resume_lock);

	/* if there's no streams to decode then exit */
	if (!inst->have_audio && !inst->have_video) {
		fprintf(stderr, "player: No streams to decode!\n");
		goto decoder_exit;
	}

	/* start decoding */
	while (LIKELY(!inst->stream_quit && av_read_frame(inst->fmt_ctx, &packet) >= 0)) {
		if (packet.stream_index == inst->video_stream_index) {
			/* if the buffer is full wait for the output thread to make room */
			while (UNLIKELY(inst->video_packet_state[inst->video_packet_write_index] == 1)) {
				pthread_mutex_lock(&inst->video_decoder_lock);
				if (UNLIKELY(inst->stream_quit)) {
					pthread_mutex_unlock(&inst->video_decoder_lock);
					goto decoder_exit;
				}
				if (LIKELY(inst->video_packet_state[inst->video_packet_write_index] == 1)) {
					pthread_cond_wait(&inst->video_decoder_signal, &inst->video_decoder_lock);
					pthread_mutex_unlock(&inst->video_decoder_lock);
					if (inst->stream_quit) {
						goto decoder_exit;
					}
					continue;
				}
				/* unlocked bellow */
			}
			
			/* save the packet and signal decoder thread */
			inst->video_packet[inst->video_packet_write_index] = packet;
			inst->video_packet_state[inst->video_packet_write_index] = 1;
			pthread_cond_signal(&inst->video_decoder_signal);
			pthread_cond_signal(&inst->video_output_signal);
			pthread_mutex_unlock(&inst->video_decoder_lock);

			inst->video_packet_write_index++;
			inst->video_packet_write_index %= MB_VIDEO_BUFFER_PACKETS;
			__sync_fetch_and_add(&inst->video_packets, 1);

		} else if (packet.stream_index == inst->audio_stream_index) {
			while (UNLIKELY(inst->audio_packet_state[inst->audio_packet_write_index] == 1)) {
				pthread_mutex_lock(&inst->audio_decoder_lock);
				if (UNLIKELY(inst->stream_quit)) {
					pthread_mutex_unlock(&inst->audio_decoder_lock);
					goto decoder_exit;
				}
				if (LIKELY(inst->audio_packet_state[inst->audio_packet_write_index] == 1)) {
					pthread_cond_wait(&inst->audio_decoder_signal, &inst->audio_decoder_lock);
					pthread_mutex_unlock(&inst->audio_decoder_lock);
					if (inst->stream_quit) {
						goto decoder_exit;
					}
					continue;
				}
				/* unlock below */
			}
			/* save the packet and signal decoder */
			inst->audio_packet[inst->audio_packet_write_index] = packet;
			inst->audio_packet_state[inst->audio_packet_write_index] = 1;
			pthread_cond_signal(&inst->audio_decoder_signal);
			pthread_cond_signal(&inst->audio_signal);
			pthread_mutex_unlock(&inst->audio_decoder_lock);

			inst->audio_packet_write_index++;
			inst->audio_packet_write_index %= MB_AUDIO_BUFFER_PACKETS;
			__sync_fetch_and_add(&inst->audio_packets, 1);
		}

		/* handle seek request */
		if (inst->seek_to != -1) {

			DEBUG_VPRINT("player", "Seeking to %li...", inst->seek_to);

			if (avformat_seek_file(inst->fmt_ctx, -1, INT64_MIN, inst->seek_to, INT64_MAX, 0) < 0) {
				fprintf(stderr, "player: Error seeking\n");
				inst->seek_result = -1;
			} else {
				inst->seek_result = 0;

				/* flush buffers */
			}

			inst->seek_to = -1;


		}
	}

decoder_exit:
	DEBUG_PRINT("player", "Stream decoder exiting");

	/* clean video stuff */
	if (inst->have_video) {
		/* signal the video thread to exit and join it */
		if (inst->video_playback_running) {
			inst->video_quit = 1;
			pthread_cond_signal(&inst->video_output_signal);
			pthread_cond_signal(&inst->video_output_signal);
			pthread_join(inst->video_output_thread, NULL);
			DEBUG_PRINT("player", "Video playback thread exited");
		}

		/* signal the video decoder thread to exit and join it */
		/* NOTE: Since this thread it's a midleman it waits on both locks */
		inst->video_decoder_quit = 1;
		pthread_cond_broadcast(&inst->video_decoder_signal);
		pthread_cond_broadcast(&inst->video_decoder_signal);
		pthread_cond_broadcast(&inst->video_output_signal);
		pthread_cond_broadcast(&inst->video_output_signal);

		for (i = 0; i < MB_VIDEO_BUFFER_PACKETS; i++) {
			if (inst->video_packet_state[i] == 1) {
				av_free_packet(&inst->video_packet[i]);
			}
		}

		pthread_join(inst->video_decoder_thread, NULL);
		DEBUG_PRINT("player", "Video decoder thread exited");
	}

	/* clean audio stuff */
	if (inst->have_audio) {
		if (inst->audio_playback_running) {
			/* signal and wait for the audio thread to exit */
			inst->audio_quit = 1;
			pthread_cond_signal(&inst->resume_signal);
			pthread_cond_signal(&inst->audio_signal);
			pthread_join(inst->audio_thread, NULL);
			DEBUG_PRINT("player", "Audio player exited");
		}

		/* signal the audio decoder thread to exit and join it */
		inst->audio_decoder_quit = 1;
		pthread_cond_broadcast(&inst->audio_decoder_signal);
		pthread_cond_broadcast(&inst->audio_signal);
		pthread_join(inst->audio_decoder_thread, NULL);
		DEBUG_PRINT("player", "Audio decoder exiting");

		for (i = 0; i < MB_AUDIO_BUFFER_PACKETS; i++) {
			if (inst->audio_packet_state[i] == 1) {
				av_free_packet(&inst->audio_packet[i]);
			}
		}

		for (i = 0; i < MB_AUDIO_BUFFER_FRAMES; i++) {
			av_free(inst->audio_frame[i]);
		}
	}

	/* clean other stuff */
	if (inst->fmt_ctx != NULL) {
		avformat_close_input(&inst->fmt_ctx);
		inst->fmt_ctx = NULL;
	}

	inst->video_stream_index = -1;
	inst->audio_stream_index = -1;
	inst->stream_quit = 0;

	mb_player_updatestatus(inst, MB_PLAYER_STATUS_READY);

	/* I don't think there's any benefit in doing this always
	 * but it helps in debugging as all freed memory is returned to
	 * the kernel so we get a better picture */
#if !defined(NDEBUG) && defined(HAVE_MALLOC_TRIM)
	malloc_trim(0);
#endif

	return NULL;
}


/**
 * mb_player_getstatus() -- Get the player status
 */
enum mb_player_status
mb_player_getstatus(struct mbp *inst)
{
	return inst->status;
}


int
mb_player_add_status_callback(struct mbp *inst, mb_player_status_callback callback)
{
	if (inst->status_callback != NULL) {
		abort(); /* only one callback supported for now */
	}

	inst->status_callback = callback;
	return 0;
}


/**
 * mb_player_seek_chapter() -- Seek to a chapter.
 */
int
mb_player_seek_chapter(struct mbp *inst, int incr)
{
	int i;
	int64_t pos;

	assert(inst != NULL);

	if (inst->status != MB_PLAYER_STATUS_PLAYING &&
		inst->status != MB_PLAYER_STATUS_PAUSED) {
		return -1;
	}

	assert(inst->fmt_ctx != NULL);
	assert(inst->getmastertime != NULL);

	pos = inst->getmastertime(inst);

	for (i = 0; i < inst->fmt_ctx->nb_chapters; i++) {
		AVChapter *ch = inst->fmt_ctx->chapters[i];
		if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
			i--;
			break;
		}
	}

	i += incr;
	if (i < 0 || i > inst->fmt_ctx->nb_chapters) {
		return -1;
	}

	int64_t seek_to = av_rescale_q(inst->fmt_ctx->chapters[i]->start,
		inst->fmt_ctx->chapters[i]->time_base, AV_TIME_BASE_Q);
	int64_t offset = seek_to - pos;

	fprintf(stderr, "player: pos=%li, seekto=%li, offset=%li\n",
		pos, seek_to, offset);

	if (inst->have_audio) {
		inst->audio_pause_requested = 1;
		while (!inst->audio_quit && inst->audio_pause_requested == 1) {
			usleep(1000);
		}
		inst->audio_clock_offset = seek_to;
		inst->seek_to = seek_to;
		mb_player_dumpvideo(inst, 1);
		mb_player_flushaudio(inst);
		pthread_cond_broadcast(&inst->resume_signal);

	} else {
		inst->systemtimeoffset += (seek_to - pos);
	}

	pos = inst->getmastertime(inst);
	fprintf(stderr, "player: newpos=%li\n", pos);

	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		mb_player_play(inst, NULL);
	}

	/* signal and wait for seek to happen */
	while (inst->seek_to != -1) {
		usleep(1000);
	}

	return inst->seek_result;
}


unsigned int
mb_player_bufferstate(struct mbp *inst)
{
	assert(inst != NULL);
	return inst->stream_percent;
}


const char *
mb_player_getmediafile(struct mbp *inst)
{
	assert(inst != NULL);
	return inst->media_file;
}


/**
 * mb_player_dismiss_top_overlay() -- Callback function to dismiss
 * overlay text
 */
static enum mbt_result
mb_player_dismiss_top_overlay(int timer_id, void *data)
{
	struct mbp *inst = (struct mbp*) data;

	assert(inst != NULL);

	pthread_mutex_lock(&inst->top_overlay_lock);

	if (inst->top_overlay_timer_id != 0) {
		DEBUG_VPRINT("player", "Dismissing top overlay for %s",
			inst->media_file);

		if (inst->top_overlay_text != NULL) {
			free(inst->top_overlay_text);
		}

		inst->top_overlay_text = NULL;
		inst->top_overlay_timer_id = 0;
	}

	pthread_mutex_unlock(&inst->top_overlay_lock);

	return MB_TIMER_CALLBACK_RESULT_CONTINUE; /* doesn't matter for ONESHOT timers */
}


/**
 * mb_player_showoverlaytext() -- Shows overlay text on the top of the
 * screen.
 */
void
mb_player_showoverlaytext(struct mbp *inst,
	const char *text, int duration, enum mbv_alignment alignment)
{
	struct timespec tv;

	pthread_mutex_lock(&inst->top_overlay_lock);

	/* if there's an overlay text being displayed then  dismiss it first */
	if (inst->top_overlay_timer_id != 0) {
		DEBUG_PRINT("player", "Cancelling existing overlay");
		mbt_cancel(inst->top_overlay_timer_id);
		if (inst->top_overlay_text != NULL) {
			free(inst->top_overlay_text);
		}
		inst->top_overlay_timer_id = 0;
		inst->top_overlay_text = NULL;
	}

	/* register the top overlay */
	tv.tv_sec = duration;
	tv.tv_nsec = 0;
	inst->top_overlay_alignment = alignment;
	inst->top_overlay_text = strdup(text);
	inst->top_overlay_timer_id = mbt_register(&tv, MB_TIMER_TYPE_ONESHOT,
		&mb_player_dismiss_top_overlay, inst);

	pthread_mutex_unlock(&inst->top_overlay_lock);
}


/**
 * mb_player_gettitle() -- Gets the title of the currently playing
 * media file or NULL if nothing is playing. The result needs to be
 * freed with free().
 */
char *
mb_player_gettitle(struct mbp *inst)
{
	AVDictionaryEntry *title_entry;

	if (inst == NULL || inst->fmt_ctx == NULL || inst->fmt_ctx->metadata == NULL) {
		return NULL;
	}

	if ((title_entry = av_dict_get(inst->fmt_ctx->metadata, "title", NULL, 0)) != NULL) {
		if (title_entry->value != NULL) {
			return strdup(title_entry->value);
		}
	}

	return strdup(inst->media_file);
}


/**
 * mb_player_play() -- If path is not NULL it opens the file
 * specified by path and starts playing it. If path is NULL
 * it resumes playback if we're on the PAUSED state and return
 * failure code (-1) if we're on any other state.
 */
int 
mb_player_play(struct mbp *inst, const char * const path)
{
	int last_percent;

	assert(inst != NULL);
	assert(inst->status == MB_PLAYER_STATUS_READY ||
		inst->status == MB_PLAYER_STATUS_PLAYING ||
		inst->status == MB_PLAYER_STATUS_PAUSED);

	/* if no path argument was provided but we're already
	 * playing a file and we're paused then just resume
	 * playback */
	if (path == NULL) {
		if (inst->status == MB_PLAYER_STATUS_PAUSED) {
			mb_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);
			if (inst->have_audio) {
				while (inst->audio_paused) {
					pthread_cond_broadcast(&inst->resume_signal);
					usleep(5000);
				}
			} else {
				mb_player_resetsystemtime(inst, inst->video_decoder_pts);
				inst->video_paused = 0;
			}
			return 0;
		}
		fprintf(stderr, "player: mb_player_play() failed -- NULL path\n");
		return -1;
	}

	if (inst->audio_paused) {
		pthread_cond_broadcast(&inst->resume_signal);
	}

	/* if we're already playing a file stop it first */
	if (inst->status != MB_PLAYER_STATUS_READY) {
		mb_player_stop(inst);
	}

	/* initialize player object */
	const char *old_media_file = inst->media_file;
	inst->media_file = strdup(path);
	if (old_media_file != NULL) {
		free((void*) old_media_file);
	}

	/* update status */
	inst->stream_percent = last_percent = 0;
	mb_player_updatestatus(inst, MB_PLAYER_STATUS_BUFFERING);

	/* start the main decoder thread */
	pthread_mutex_lock(&inst->resume_lock);
	if (pthread_create(&inst->thread, NULL, mb_player_stream_decode, inst) != 0) {
		fprintf(stderr, "pthread_create() failed!\n");
		mb_player_updatestatus(inst, MB_PLAYER_STATUS_READY);
		return -1;
	}
	pthread_cond_wait(&inst->resume_signal, &inst->resume_lock);
	pthread_mutex_unlock(&inst->resume_lock);

	/* if there's no audio or video to decode then return error and exit.
	 * The decoder thread will shut itself down so no cleanup is necessary */
	if (!inst->have_audio && !inst->have_video) {
		return -1;
	}

	/* wait for the buffers to fill up */
	while (inst->audio_frames < MB_AUDIO_BUFFER_FRAMES &&
		inst->video_frames < MB_VIDEO_BUFFER_FRAMES) {

		/* update progressbar */
		int avail = inst->video_frames + inst->audio_frames;
		const int wanted = MB_AUDIO_BUFFER_FRAMES + MB_VIDEO_BUFFER_FRAMES;
		inst->stream_percent = (((avail * 100) / wanted) * 100) / 100;

		if (inst->stream_percent != last_percent) {
			mb_player_updatestatus(inst, MB_PLAYER_STATUS_BUFFERING);
			last_percent = inst->stream_percent;
		}

		mb_player_printstatus(inst, 0);
		usleep(5000);
	}

	/* we're done buffering, set state to PLAYING */
	mb_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);

	/* show title on top overlay */
	char *title = mb_player_gettitle(inst);
	if (title != NULL) {
		mb_player_showoverlaytext(inst, title, 15,
			MBV_ALIGN_CENTER);
		free(title);
	}

	DEBUG_PRINT("player", "Firing rendering threads");

	/* fire the video threads */
	pthread_mutex_lock(&inst->video_output_lock);
	if (pthread_create(&inst->video_output_thread, NULL, mb_player_video, inst) != 0) {
		fprintf(stderr, "player: Could not start renderer thread\n");
		pthread_mutex_unlock(&inst->video_output_lock);
		return -1;
	}
	pthread_cond_wait(&inst->video_output_signal, &inst->video_output_lock);
	pthread_mutex_unlock(&inst->video_output_lock);

	/* fire the audio output thread */
	if (inst->have_audio) {
		pthread_mutex_lock(&inst->audio_lock);
		if (pthread_create(&inst->audio_thread, NULL, mb_player_audio, inst) != 0) {
			abort();
		}
		pthread_cond_wait(&inst->audio_signal, &inst->audio_lock);
		pthread_mutex_unlock(&inst->audio_lock);
	}


	/* detach the decoder thread */
	pthread_detach(inst->thread);

	return 0;
}


/**
 * mb_player_pause() -- Pause the stream.
 */
int
mb_player_pause(struct mbp* inst)
{
	assert(inst != NULL);

	/* can't pause if we're not playing */
	if (inst->status != MB_PLAYER_STATUS_PLAYING) {
		fprintf(stderr, "player: Cannot puase, not playing\n");
		return -1;
	}

	/* update status */
	mb_player_updatestatus(inst, MB_PLAYER_STATUS_PAUSED);

	/* wait for player to pause */
	if (inst->have_audio) {
		inst->audio_pause_requested = 1;
		while (inst->audio_paused == 0) {
			usleep(5000);
		}
	} else {
		inst->video_paused = 1;
	}

	return 0;
}


int
mb_player_stop(struct mbp* inst)
{
	/* if the video is paused then unpause it first. */
	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		fprintf(stderr, "player: Unpausing stream\n");
		mb_player_play(inst, NULL);
	}

	if (inst->have_audio) {
		while (inst->audio_paused) {
			pthread_cond_signal(&inst->resume_signal);
		}
	}

	if (inst->status != MB_PLAYER_STATUS_READY) {
		inst->stream_quit = 1;
		pthread_cond_broadcast(&inst->audio_decoder_signal);
		pthread_cond_broadcast(&inst->video_decoder_signal);

		if (inst->have_audio) {
			while (inst->audio_paused) {
				pthread_cond_broadcast(&inst->resume_signal);
				usleep(1000);
			}
		} else {
			/* video should have been taken care of by stop() */
			//inst->video_paused = 0;
		}

		while (inst->status != MB_PLAYER_STATUS_READY) {
			usleep(1000);
		}
		return 0;
	}
	fprintf(stderr, "player: Nothing to stop\n");
	return -1;
}


/**
 * mb_player_checkfbdev() -- Checks if there's a framebuffer device suitable
 * for direct rendering
 */
static void
mb_player_checkfbdev(struct mbp *inst)
{
	int fd;

	assert(inst != NULL);
	assert(inst->window != NULL);

	if (!mbv_isfbdev()) {
		fprintf(stderr, "player: WARNING!!: Direct rendering disabled\n");
		inst->use_fbdev = 0;
		return;
	}

	DEBUG_PRINT("player", "Initializing /dev/fb0");

	/* try to gain root */
	if (mb_su_gainroot() == -1) {
		fprintf(stderr, "player: Cannot gain root rights!\n");
	}

	if ((fd = open("/dev/fb0", O_RDWR)) != -1) {
		struct fb_fix_screeninfo finfo;
		struct fb_var_screeninfo vinfo;
		void *fb_mem = NULL;
		long screensize;

		/* get screeninfo */
		if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == -1 ||
			ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
		{
			fprintf(stderr, "player: mb_player_checkfbdev(): ioctl() failed\n");
			inst->use_fbdev = 0;
			goto end;
		}

		/* dump some screen info */
		DEBUG_VPRINT("player", "fbdev: bpp=%i", vinfo.bits_per_pixel);
		DEBUG_VPRINT("player", "fbdev: type=%i", finfo.type);
		DEBUG_VPRINT("player", "fbdev: visual=%i", finfo.visual);
		DEBUG_VPRINT("player", "fbdev: FOURCC (grayscale): '%c%c%c%c'",
			((char*)&vinfo.grayscale)[0], ((char*)&vinfo.grayscale)[1],
			((char*)&vinfo.grayscale)[2], ((char*)&vinfo.grayscale)[3]);
		DEBUG_VPRINT("player", "fbdev: xoffset=%i yoffset=%i r=%i g=%i b=%i"
			"player: r=%i g=%i b=%i\n",
			vinfo.xoffset, vinfo.yoffset,
			vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset,
			vinfo.red.length, vinfo.green.length, vinfo.blue.length);

		/* try to mmap video memory */
		screensize = vinfo.yres_virtual * finfo.line_length;
		fb_mem = mmap(0, screensize, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, (off_t) 0);
		if (fb_mem == MAP_FAILED) {
			fprintf(stderr, "player: mmap() failed\n");
			inst->use_fbdev = 0;
			close(fd);
			goto end;
		}

		/* framebuffer device is good */
		inst->use_fbdev = 1;

		/* unmap memory and cleanup */
		munmap(fb_mem, screensize);
		close(fd);

	} else {
		inst->use_fbdev = 0;
	}
end:
	mb_su_droproot();
}


/**
 * mb_player_init() -- Create a new player object.
 */
struct mbp*
mb_player_new(struct mbv_window *window)
{
	struct mbp* inst;
	static int initialized = 0;

	/* initialize libav */
	if (!initialized) {
		av_register_all();
		avfilter_register_all();

		pango_font_desc = pango_font_description_from_string("Sans Bold 36px");
		assert(pango_font_desc != NULL);
		initialized = 1;
	}

	/* allocate memory for the player object */
	inst = malloc(sizeof(struct mbp));
	if (inst == NULL) {
		fprintf(stderr, "mbp_init() failed -- out of memory\n");
		return NULL;
	}

	memset(inst, 0, sizeof(struct mbp));

	/* if no window argument was provided then use the root window */
	if (window == NULL) {
		window = mbv_getrootwindow();
		if (window == NULL) {
			fprintf(stderr, "player: Could not get root window\n");
			free(inst);
			return NULL;
		}
	}

	inst->window = window;
	inst->use_fbdev = 1;
	inst->video_stream_index = -1;
	inst->status = MB_PLAYER_STATUS_READY;

	/* get the size of the window */
	if (mbv_window_getsize(inst->window, &inst->width, &inst->height) == -1) {
		fprintf(stderr, "player: Could not get window size\n");
		free(inst);
		return NULL;
	}

	/* initialize pthreads primitives */
	if (pthread_mutex_init(&inst->resume_lock, NULL) != 0 ||
		pthread_mutex_init(&inst->video_output_lock, NULL) != 0 ||
		pthread_mutex_init(&inst->audio_lock, NULL) != 0 ||
		pthread_mutex_init(&inst->audio_decoder_lock, NULL) != 0 ||
		pthread_mutex_init(&inst->video_decoder_lock, NULL) != 0 ||
		pthread_cond_init(&inst->video_decoder_signal, NULL) != 0 ||
		pthread_cond_init(&inst->audio_decoder_signal, NULL) != 0 ||
		pthread_cond_init(&inst->audio_signal, NULL) != 0 ||
		pthread_cond_init(&inst->video_output_signal, NULL) != 0 ||
		pthread_cond_init(&inst->resume_signal, NULL) != 0 ||
		pthread_mutex_init(&inst->top_overlay_lock, NULL) != 0) {
		fprintf(stderr, "player: pthreads initialization failed\n");
		free(inst);
		return NULL;
	}

	/* check if the framebuffer device is usable for
	 * direct rendering */
	mb_player_checkfbdev(inst);

	return inst;
}


/**
 * mb_player_destroy() -- Destroy this player instance
 */
void
mb_player_destroy(struct mbp *inst)
{
	assert(inst != NULL);

	DEBUG_PRINT("player", "Destroying object");

	/* this just fails if we're not playing */
	(void) mb_player_stop(inst);

	if (inst->media_file != NULL) {
		free((void*) inst->media_file);
	}
	free(inst);
}

void
mb_player_shutdown()
{
	if (LIKELY(pango_font_desc != NULL)) {
		pango_font_description_free(pango_font_desc);
	}
}
