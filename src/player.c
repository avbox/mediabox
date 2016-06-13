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

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>


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
#define MB_VIDEO_BUFFER_FRAMES  (15)
#define MB_VIDEO_BUFFER_PACKETS (15)
#define MB_AUDIO_BUFFER_FRAMES  (99)
#define MB_AUDIO_BUFFER_PACKETS (1)

#define MB_DECODER_PRINT_FPS


/* render directly to fbdev */
#define MB_FBDEV_RENDERER

#define MB_ALSA_BUFFER_SIZE	(32 * 1024)
#define MB_ALSA_LATENCY		(500000)


enum mb_player_action
{
	MB_PLAYER_ACTION_NONE        = 0,
	MB_PLAYER_ACTION_PAUSE       = 1,
	MB_PLAYER_ACTION_STOP        = 2,
	MB_PLAYER_ACTION_FASTFORWARD = 4,
	MB_PLAYER_ACTION_REWIND      = 8
};


struct mbp
{
	struct mbv_window *window;
	const char *media_file;
	enum mb_player_status status;
	enum mb_player_action action;
	int frames_rendered;
	int width;
	int height;
	int last_err;
	int have_audio;
	int have_video;
	uint8_t *buf;
	int bufsz;
	uint8_t *render_mask;
	int use_fbdev;
	mb_player_status_callback status_callback;

	AVFormatContext *fmt_ctx;

	snd_pcm_t *audio_pcm_handle;
	AVFrame *audio_frame[MB_AUDIO_BUFFER_FRAMES];
	char audio_frame_state[MB_AUDIO_BUFFER_FRAMES];
	int audio_playback_index;
	int audio_decode_index;
	int audio_frames;
	int audio_packets;
	int audio_quit;
	int audio_paused;
	int64_t audio_elapsed_before_trigger;
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
	AVPacket video_packet[MB_VIDEO_BUFFER_PACKETS];
	char video_packet_state[MB_VIDEO_BUFFER_PACKETS];
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
	pthread_cond_t video_decoder_signal;
	pthread_mutex_t video_decoder_lock;
	pthread_t video_decoder_thread;
	pthread_cond_t video_output_signal;
	pthread_mutex_t video_output_lock;
	pthread_t video_output_thread;

	pthread_cond_t resume_signal;
	pthread_mutex_t resume_lock;
	pthread_t thread;
};


/**
 * mb_player_updatestatus() -- Updates the player status and
 * calls any registered callbacks
 */
static void
mb_player_updatestatus(struct mbp *inst, enum mb_player_status status)
{
	assert(inst != NULL);

	inst->status = status;
	if (inst->status_callback != NULL) {
		inst->status_callback(inst, status);
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
 * mb_player_wait4buffers() -- Waits for the decoded stream buffers
 * to fill up
 */
static void
mb_player_wait4buffers(struct mbp *inst)
{
	/* wait for the buffers to fill up */
	while (inst->video_frames < MB_VIDEO_BUFFER_FRAMES &&
		inst->audio_frames < MB_VIDEO_BUFFER_FRAMES) {
		mb_player_printstatus(inst, 0);
		usleep(5000);
	}
}


/**
 * mb_player_getaudiotime() -- Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
static int64_t
mb_player_getaudiotime(struct mbp* inst)
{
	int err;
	uint64_t time;
	snd_pcm_status_t *status;
	snd_htimestamp_t audio_timestamp, audio_trigger_timestamp;

	if (inst->audio_paused) {
		return inst->audio_elapsed_before_trigger;
	}

	snd_pcm_status_alloca(&status);

	if ((err = snd_pcm_status(inst->audio_pcm_handle, status)) < 0) {
		printf("Stream status error: %s\n", snd_strerror(err));
		return 0;
	}

	snd_pcm_status_get_trigger_htstamp(status, &audio_trigger_timestamp);
	snd_pcm_status_get_htstamp(status, &audio_timestamp);

	time  = ((audio_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_timestamp.tv_nsec) / 1000;
	time -= ((audio_trigger_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_trigger_timestamp.tv_nsec) / 1000;
	time += inst->audio_elapsed_before_trigger;

	return (int64_t) time;
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
	snd_htimestamp_t audio_timestamp, audio_trigger_timestamp;
	snd_pcm_uframes_t avail;

	snd_pcm_status_alloca(&status);

	if ((err = snd_pcm_status(inst->audio_pcm_handle, status)) < 0) {
		printf("Stream status error: %s\n", snd_strerror(err));
		return -1;
	}

	snd_pcm_status_get_trigger_htstamp(status, &audio_trigger_timestamp);
	snd_pcm_status_get_htstamp(status, &audio_timestamp);

	time  = ((audio_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_timestamp.tv_nsec) / 1000;
	time -= ((audio_trigger_timestamp.tv_sec * 1000 * 1000 * 1000) + audio_trigger_timestamp.tv_nsec) / 1000;
	time += inst->audio_elapsed_before_trigger;

	/* set the clock to the current audio time */
	inst->audio_elapsed_before_trigger = time;
	inst->audio_paused = 1;
	avail = snd_pcm_status_get_avail(status);

	/* move the clock up to what it should be after the ALSA buffer is drained
	 * and drain the buffer */
	inst->audio_elapsed_before_trigger += ((1000 * 1000) / inst->audio_framerate) * (inst->audio_buffer_size - avail);
	snd_pcm_drain(inst->audio_pcm_handle);

	return 0;
}


/**
 * mb_player_resumeaudio() -- Resume audio playback
 */
static int
mb_player_resumeaudio(struct mbp *inst)
{
	inst->audio_paused = 0;
	//snd_pcm_pause(inst->audio_pcm_handle, 0);
	snd_pcm_reset(inst->audio_pcm_handle);
	snd_pcm_prepare(inst->audio_pcm_handle);
	return 0;
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

	assert(inst != NULL);
	assert(inst->audio_pcm_handle == NULL);
	assert(inst->audio_quit == 0);
	assert(inst->audio_paused == 0);

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_sw_params_alloca(&swparams);

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
		return NULL;
	}
	#if 1
	if ((ret = snd_pcm_hw_params_set_period_size_near(inst->audio_pcm_handle, params, &period_frames, &dir)) < 0) {
		fprintf(stderr, "player: Cannot set period. %s\n",
			snd_strerror(ret));
		return NULL;
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
		return NULL;
	}

	if ((ret = snd_pcm_sw_params_current(inst->audio_pcm_handle, swparams)) < 0) {
		fprintf(stderr, "player: Could not determine SW params. %s\n",
			snd_strerror(ret));
		return NULL;
	}
	if ((ret = snd_pcm_sw_params_set_tstamp_type(inst->audio_pcm_handle, swparams, SND_PCM_TSTAMP_TYPE_MONOTONIC)) < 0) {
		fprintf(stderr, "player: Could not set ALSA clock to CLOCK_MONOTONIC. %s\n",
			snd_strerror(ret));
		return NULL;
	}
	if ((ret = snd_pcm_sw_params(inst->audio_pcm_handle, swparams)) < 0) {
		fprintf(stderr, "player: Could not set ALSA SW paramms. %s\n",
			snd_strerror(ret));
		return NULL;
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

	fprintf(stderr, "player: ALSA buffer size: %lu\n", (unsigned long) inst->audio_buffer_size);
	fprintf(stderr, "player: ALSA period size: %lu\n", (unsigned long) period_frames);
	fprintf(stderr, "player: ALSA period time: %u\n", period_usecs);
	fprintf(stderr, "player: ALSA framerate: %u\n", inst->audio_framerate);

	(void) mb_su_droproot();

	/* signal video thread that we're ready to start */
	pthread_mutex_lock(&inst->audio_lock);
	pthread_cond_broadcast(&inst->audio_signal);
	pthread_mutex_unlock(&inst->audio_lock);

	fprintf(stderr, "player: Audio thread ready\n");

	/* start decoding */
	while (inst->audio_quit == 0) {

		/* if there's no frame ready we must wait */
		if (inst->audio_frame_state[inst->audio_playback_index] != 1) {
			pthread_mutex_lock(&inst->audio_lock);
			if (inst->audio_quit) {
				pthread_mutex_unlock(&inst->audio_lock);
				continue;
			}
			if (inst->audio_frame_state[inst->audio_playback_index] != 1) {
				#if 0
				/* fprintf(stderr, "Audio thread waiting\n"); */
				pthread_cond_wait(&inst->audio_signal, &inst->audio_lock);
				pthread_mutex_unlock(&inst->audio_lock);
				continue;
				#else
				pthread_mutex_unlock(&inst->audio_lock);
				fprintf(stderr, "player: Audio stalled\n");
				mb_player_pauseaudio(inst);
				mb_player_wait4buffers(inst);
				mb_player_resumeaudio(inst);
				continue;
				#endif
			}
			pthread_mutex_unlock(&inst->audio_lock);
			if (inst->audio_quit) {
				continue;
			}
		}

		/* pause */
		if (inst->action != MB_PLAYER_ACTION_NONE) {
			if (inst->action & MB_PLAYER_ACTION_PAUSE) {
				fprintf(stderr, "player: Audio pausing\n");
				mb_player_pauseaudio(inst);
				pthread_mutex_lock(&inst->resume_lock);
				pthread_cond_wait(&inst->resume_signal, &inst->resume_lock);
				pthread_mutex_unlock(&inst->resume_lock);
				mb_player_resumeaudio(inst);
			}
		}


		/* play the frame */
		frames = snd_pcm_writei(inst->audio_pcm_handle, inst->audio_frame[inst->audio_playback_index]->data[0],
			inst->audio_frame[inst->audio_playback_index]->nb_samples);
		if (frames < 0) {
			frames = snd_pcm_recover(inst->audio_pcm_handle, frames, 0);
		}
		if (frames < 0) {
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
		pthread_mutex_unlock(&inst->audio_lock);
		pthread_cond_signal(&inst->audio_signal);
		pthread_cond_broadcast(&inst->audio_decoder_signal);

		inst->audio_playback_index++;
		inst->audio_playback_index %= MB_AUDIO_BUFFER_FRAMES;

		//inst->audio_frames--;
		__sync_fetch_and_sub(&inst->audio_frames, 1);

	}

audio_exit:
	fprintf(stderr, "player: Audio thread exiting\n");

	/* cleanup */
	if (inst->audio_pcm_handle == NULL) {
		snd_pcm_close(inst->audio_pcm_handle);
		inst->audio_pcm_handle = NULL;
	}

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
	uint8_t *m = (uint8_t*) mbv_dfb_getscreenmask();

	pixelsz = vinfo->bits_per_pixel / CHAR_BIT;
	
	(void) ioctl(fd, FBIO_WAITFORVSYNC, &screen);

	for (y = 0; y < vinfo->yres; y++) {
		for (x = 0; x < vinfo->xres; x++) {
			if (!m[(inst->width * y) + x]) {
				long location = (x + vinfo->xoffset) * pixelsz + (y + vinfo->yoffset) * finfo->line_length;
				uint32_t *ppix = (uint32_t*) buf;
				*((uint32_t*)(fb_mem + location)) = *(ppix + (((inst->width * y) + x)));
			}
		}
	}
}


static int ielapsed = 0;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
void
interval_handler(int signum)
{
	struct itimerval itimer;
	memset(&itimer, 0, sizeof(itimer));
	setitimer(ITIMER_REAL, &itimer, NULL);
	ielapsed = 1;
	pthread_cond_signal(&wait_cond);

}

static int
mb_player_sleep(int64_t usecs)
{
	struct sigaction sa;
	struct itimerval itimer;
	struct timespec t1, t2;

	(void) clock_gettime(CLOCK_MONOTONIC, &t1);


	memset (&sa, 0, sizeof (sa));
	sa.sa_handler = &interval_handler;
	sigaction (SIGALRM, &sa, NULL);
	itimer.it_interval.tv_sec = 0;
	itimer.it_interval.tv_usec = usecs;
	itimer.it_value.tv_sec = 0;
	itimer.it_value.tv_usec = usecs;
	ielapsed = 0;

	if (setitimer(ITIMER_REAL, &itimer, NULL) == -1) {
		fprintf(stderr, "setitimer failed errno=%i\n", errno);
		return -1;
	}

	do {
		pthread_mutex_lock(&wait_lock);
		pthread_cond_wait(&wait_cond, &wait_lock);
		pthread_mutex_unlock(&wait_lock);
	} while (!ielapsed);
	memset(&itimer, 0, sizeof(itimer));
	setitimer(ITIMER_REAL, &itimer, NULL);

	(void) clock_gettime(CLOCK_MONOTONIC, &t2);

	return utimediff(&t1, &t2);
}


/**
 * mb_player_wait4audio() -- Waits for the audio stream to start playing
 */
static void
mb_player_wait4audio(struct mbp* inst)
{
	int ret;
	snd_pcm_status_t *status;
	snd_pcm_status_alloca(&status);
	while (inst->audio_pcm_handle == NULL) {
		usleep(5000);
	}
	do {
		if ((ret = snd_pcm_status(inst->audio_pcm_handle, status)) < 0) {
			fprintf(stderr, "player: Could not get ALSA status\n");
		}
		usleep(1); /* do not raise this value */
	}
	while (snd_pcm_status_get_state(status) != SND_PCM_STATE_RUNNING);
}


/**
 * mb_player_vrend_thread() -- Video rendering thread.
 */
#if (MB_VIDEO_BUFFER_FRAMES > 1)
static void *
mb_player_video(void *arg)
{
	uint8_t *buf;
	int64_t frame_pts, delay, frame_time;
	struct timespec ref_timestamp, timestamp;
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

	assert(inst != NULL);
	assert(inst->video_quit == 0);

	fprintf(stderr, "player: Video renderer started\n");

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

	fprintf(stderr, "player: Video renderer ready\n");

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
		mb_player_wait4audio(inst);
	} else {
		/* save the reference timestamp */
		(void) clock_gettime(CLOCK_MONOTONIC, &ref_timestamp);
	}

	while (!inst->video_quit) {
		/* if there's no frame ready we must wait */
		if (inst->frame_state[inst->video_playback_index] != 1) {
			pthread_mutex_lock(&inst->video_output_lock);
			if (inst->video_quit) {
				pthread_mutex_unlock(&inst->video_output_lock);
				goto video_exit;
			}
			if (inst->frame_state[inst->video_playback_index] != 1) {
				fprintf(stderr, "player: Waiting for video decoder\n");
				if (inst->have_audio) {
					inst->action |= MB_PLAYER_ACTION_PAUSE;
					pthread_cond_wait(&inst->video_output_signal, &inst->video_output_lock);
					pthread_mutex_unlock(&inst->video_output_lock);
					mb_player_wait4buffers(inst);
					inst->action &= ~MB_PLAYER_ACTION_PAUSE;
					while (inst->audio_paused) {
						pthread_cond_broadcast(&inst->resume_signal);
						usleep(1000);
					}
				} else {
					pthread_cond_wait(&inst->video_output_signal, &inst->video_output_lock);
					pthread_mutex_unlock(&inst->video_output_lock);
				}
				continue;
			}
			pthread_mutex_unlock(&inst->video_output_lock);
		}

		frame_pts = inst->frame_pts[inst->video_playback_index];

		if  (frame_pts != AV_NOPTS_VALUE) {
			int64_t elapsed;

			frame_time = av_rescale_q(frame_pts,
				inst->frame_time_base[inst->video_playback_index], AV_TIME_BASE_Q);
			/* delay += inst->frame_repeat[inst->video_playback_index] * delay; */

recalc:
			if (inst->video_quit) {
				continue;
			}
			if (inst->have_audio) {
				/* getaudiotime() should (when it's bug free) always give us
				 * the time since the audio stream started playing, excluding
				 * paused time so it's all we need */
				elapsed = mb_player_getaudiotime(inst);
				if (elapsed > frame_time) {
					delay = 0;
					if (elapsed - frame_time > 100000) {
						fprintf(stderr, "player: skipping frame_time=%li elapsed=%li diff=%li\n",
							frame_time, elapsed, elapsed - frame_time);
						/* skip frame */
						goto frame_complete;
					}
				} else {
					delay = frame_time - elapsed;
				}
			} else {
				abort();
				(void) clock_gettime(CLOCK_MONOTONIC, &timestamp);
				elapsed = utimediff(&timestamp, &ref_timestamp);
				delay = frame_time - elapsed;
				if (delay < -1000) {
					goto frame_complete;
				}
			}

			delay &= ~0xFF;	/* a small delay  will only waste time (context switch
					   time + recalc time) and we'll loose time because we're not
					   likely to wake up that quick */

			if (delay > 0 && delay < 1000000) {
				mb_player_sleep(delay);

				/* the clock may have been stopped while we slept */
				goto recalc;

			} else if (delay < 0) {
				if (delay < -1000) {
					goto frame_complete;
				}
			}
		}

		buf = inst->frame_data[inst->video_playback_index];

		if (inst->use_fbdev) {
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
		if (elapsed_tp.tv_sec > 0) {
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
		pthread_mutex_unlock(&inst->video_output_lock);
		pthread_cond_signal(&inst->video_output_signal);
		pthread_cond_broadcast(&inst->video_decoder_signal);

		inst->video_playback_index++;
		inst->video_playback_index %= MB_VIDEO_BUFFER_FRAMES;

		/* inst->video_frames--; */
		__sync_fetch_and_sub(&inst->video_frames, 1);

		/* pause */
		if (!inst->have_audio && inst->action != MB_PLAYER_ACTION_NONE) {
			if (inst->action & MB_PLAYER_ACTION_PAUSE) {
				fprintf(stderr, "player: Video pausing\n");
				pthread_mutex_lock(&inst->resume_lock);
				inst->video_paused = 1;
				pthread_cond_wait(&inst->resume_signal, &inst->resume_lock);
				inst->video_paused = 0;
				pthread_mutex_unlock(&inst->resume_lock);

				/* TODO: THIS IS WRONG */
				(void) clock_gettime(CLOCK_MONOTONIC, &ref_timestamp);
			}
		}
	}

video_exit:
	fprintf(stderr, "player: Video renderer exiting\n");

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

	return NULL;
	
}
#endif


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
	uint8_t *buf = NULL;
	struct mbp *inst = (struct mbp*) arg;
	char video_filters[512];
	AVFrame *video_frame_nat = NULL, *video_frame_flt = NULL;
	AVCodecContext *video_codec_ctx = NULL;
	AVFilterGraph *video_filter_graph = NULL;
	AVFilterContext *video_buffersink_ctx = NULL;
	AVFilterContext *video_buffersrc_ctx = NULL;

	MB_DEBUG_SET_THREAD_NAME("video_decode");

	assert(inst != NULL);
	assert(inst->fmt_ctx != NULL);
	assert(inst->video_stream_index == -1);

	/* open the video codec */
	if (open_codec_context(&inst->video_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
		video_codec_ctx = inst->fmt_ctx->streams[inst->video_stream_index]->codec;	
	}
	if (video_codec_ctx == NULL) {
		goto decoder_exit;
	}

	/* initialize video filter graph */
	snprintf(video_filters, sizeof(video_filters),
		"scale='if(gt(a,4/3),%i,-1)':'if(gt(a,4/3),-1,%i)',"
		"pad=%i:%i:'((out_w - in_w) / 2)':'((out_h - in_h) / 2)'",
		inst->width, inst->height, inst->width, inst->height);
	fprintf(stderr, "player: video_filters: %s\n",
		video_filters);
	if (mb_player_initvideofilters(inst->fmt_ctx, video_codec_ctx,
		&video_buffersink_ctx, &video_buffersrc_ctx, &video_filter_graph,
		video_filters, inst->video_stream_index) < 0) {
		fprintf(stderr, "player: Could not init filter graph!\n");
		goto decoder_exit;
	}

	/* calculate the size of each frame and allocate buffer for it */
	/* TODO: Make sure buffers are aligned */
	inst->bufsz = avpicture_get_size(MB_DECODER_PIX_FMT, inst->width, inst->height);
	inst->buf = buf = av_malloc(inst->bufsz * sizeof(uint8_t) * MB_VIDEO_BUFFER_FRAMES);
	if (buf == NULL) {
		fprintf(stderr, "player: Could not allocate buffer\n");
		goto decoder_exit;
	}

	for (i = 0; i < MB_VIDEO_BUFFER_FRAMES; i++) {
		inst->frame_data[i] = buf + (i * inst->bufsz);
		inst->frame_state[i] = 0;
	}

	fprintf(stderr, "player: video_codec_ctx: width=%i height=%i pix_fmt=%i\n",
		inst->width, inst->height, video_codec_ctx->pix_fmt);

	/* allocate video frames */
	video_frame_nat = av_frame_alloc(); /* native */
	video_frame_flt = av_frame_alloc(); /* filtered */
	if (video_frame_nat == NULL || video_frame_flt == NULL) {
		fprintf(stderr, "player: Could not allocate frames\n");
		goto decoder_exit;
	}

	fprintf(stderr, "player: Video decoder ready\n");

	/* signal control trhead that we're ready */
	pthread_mutex_lock(&inst->video_decoder_lock);
	pthread_cond_signal(&inst->video_decoder_signal);
	pthread_mutex_unlock(&inst->video_decoder_lock);


	while (!inst->video_decoder_quit) {
		/* if there's no frame to decode wait */
		if (inst->video_packet_state[inst->video_packet_read_index] != 1) {
			pthread_mutex_lock(&inst->video_decoder_lock);
			if (inst->video_decoder_quit) {
				pthread_mutex_unlock(&inst->video_decoder_lock);
				goto decoder_exit;
			}
			if (inst->video_packet_state[inst->video_packet_read_index] != 1) {
				pthread_cond_wait(&inst->video_decoder_signal, &inst->video_decoder_lock);
				pthread_mutex_unlock(&inst->video_decoder_lock);
				continue;
			}
			pthread_mutex_unlock(&inst->video_decoder_lock);
			if (inst->video_decoder_quit) {
				continue;
			}
		}

		/* decode frame */
		if ((i = avcodec_decode_video2(video_codec_ctx, video_frame_nat, &finished, &inst->video_packet[inst->video_packet_read_index])) < 0) {
			fprintf(stderr, "player: avcodec_decode_video2() returned %i\n", i);
		}

		if (finished) {
			int64_t frame_pts = video_frame_nat->pts =
				av_frame_get_best_effort_timestamp(video_frame_nat);

			/* push the decoded frame into the filtergraph */
			if (av_buffersrc_add_frame_flags(video_buffersrc_ctx,
				video_frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
				fprintf(stderr, "player: Error feeding filterchain\n");
				goto decoder_exit;
			}

			/* pull filtered frames from the filtergraph */
			while (1) {
				i = av_buffersink_get_frame(video_buffersink_ctx, video_frame_flt);
				if (i == AVERROR(EAGAIN) || i == AVERROR_EOF) {
					break;
				}
				if (i < 0) {
					goto decoder_exit;
				}

				/* if the renderer has not finished we must wait */
				while (inst->frame_state[inst->video_decode_index] != 0) {
					pthread_mutex_lock(&inst->video_output_lock);
					if (inst->video_decoder_quit) {
						pthread_mutex_unlock(&inst->video_output_lock);
						goto decoder_exit;
					}
					if (inst->frame_state[inst->video_decode_index] != 0) {
						/*fprintf(stderr, "player: "
							"Waiting for renderer\n"); */
						pthread_cond_wait(&inst->video_output_signal,
							&inst->video_output_lock);
					}
					pthread_mutex_unlock(&inst->video_output_lock);
					if (inst->video_decoder_quit) {
						goto decoder_exit;
					}
				}

				/* copy picture to buffer */
				avpicture_layout((const AVPicture*) video_frame_flt,
					MB_DECODER_PIX_FMT, inst->width, inst->height,
					inst->frame_data[inst->video_decode_index], inst->bufsz);


				/* update the buffer index and signal renderer thread */
				pthread_mutex_lock(&inst->video_output_lock);
				inst->frame_repeat[inst->video_decode_index] = video_frame_flt->repeat_pict;
				inst->frame_state[inst->video_decode_index] = 1;
				inst->frame_pts[inst->video_decode_index] = frame_pts;
				inst->frame_time_base[inst->video_decode_index] = 
					video_buffersink_ctx->inputs[0]->time_base;
				inst->video_decode_index++;
				inst->video_decode_index %= MB_VIDEO_BUFFER_FRAMES;
				pthread_cond_signal(&inst->video_output_signal);
				pthread_mutex_unlock(&inst->video_output_lock);

				av_frame_unref(video_frame_flt);

				/* inst->video_frames++; */
				__sync_fetch_and_add(&inst->video_frames, 1);

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
	fprintf(stderr, "player: Video decoder exiting\n");

	if (buf != NULL) {
		av_free(buf);
		inst->buf = NULL;
	}
	if (video_frame_nat != NULL) {
		av_free(video_frame_nat);
	}
	if (video_frame_flt != NULL) {
		av_free(video_frame_flt);
	}
	if (video_codec_ctx != NULL) {
		avcodec_close(video_codec_ctx);
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

	fprintf(stderr, "player: Audio decoder starting\n");

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

	/* initialize video filter graph */
	fprintf(stderr, "player: audio_filters: %s\n",
		audio_filters);
	if (mb_player_initaudiofilters(inst->fmt_ctx, audio_codec_ctx,
		&audio_buffersink_ctx, &audio_buffersrc_ctx, &audio_filter_graph,
		audio_filters, inst->audio_stream_index) < 0) {
		fprintf(stderr, "player: Could not init filter graph!\n");
		goto decoder_exit;
	}

	/* signl control thread that we're ready */
	fprintf(stderr, "player: Audio decoder ready\n");
	pthread_mutex_lock(&inst->audio_decoder_lock);
	pthread_cond_signal(&inst->audio_decoder_signal);
	pthread_mutex_unlock(&inst->audio_decoder_lock);


	while (!inst->audio_decoder_quit) {
		/* wait for the stream decoder to give us some packets */
		if (inst->audio_packet_state[inst->audio_packet_read_index] != 1) {
			pthread_mutex_lock(&inst->audio_decoder_lock);
			if (inst->audio_decoder_quit) {
				pthread_mutex_unlock(&inst->audio_decoder_lock);
				continue;
			}
			if  (inst->audio_packet_state[inst->audio_packet_read_index] != 1) {
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
		while (packet1.size > 0) {
			finished = 0;
			ret = avcodec_decode_audio4(audio_codec_ctx, audio_frame_nat,
				&finished, &packet1);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Error decoding audio\n");
				continue;
			}
			packet1.size -= ret;
			packet1.data += ret;

			if (finished) {
				/* push the audio data from decoded frame into the filtergraph */
				if (av_buffersrc_add_frame_flags(audio_buffersrc_ctx,
					audio_frame_nat, 0) < 0) {
					av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph\n");
					break;
				}

				/* pull filtered audio from the filtergraph */
				while (!inst->audio_decoder_quit) {

					/* if the video output has not finished we must wait */
					if (inst->audio_frame_state[inst->audio_decode_index] != 0) {
						pthread_mutex_lock(&inst->audio_lock);
						if (inst->audio_decoder_quit) {
							pthread_mutex_unlock(&inst->audio_lock);
							continue;
						}
						if (inst->audio_frame_state[inst->audio_decode_index] != 0) {
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
					if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
						av_frame_unref(inst->audio_frame[inst->audio_decode_index]);
						break;
					}
					if (ret < 0) {
						av_frame_unref(inst->audio_frame[inst->audio_decode_index]);
						goto decoder_exit;
					}

					/* update the buffer index and signal renderer thread */
					pthread_mutex_lock(&inst->audio_lock);
					inst->audio_frame_state[inst->audio_decode_index] = 1;
					inst->audio_decode_index++;
					inst->audio_decode_index %= MB_AUDIO_BUFFER_FRAMES;
					pthread_cond_signal(&inst->audio_signal);
					pthread_mutex_unlock(&inst->audio_lock);

					/* inst->audio_frames++; */
					__sync_fetch_and_add(&inst->audio_frames, 1);
				}
			}
		}
		/* free packet */
		av_free_packet(&packet);
	}

decoder_exit:
	fprintf(stderr, "player: Audio decoder exiting\n");

	if (audio_frame_nat != NULL) {
		av_free(audio_frame_nat);
	}
	if (audio_codec_ctx != NULL) {
		avcodec_close(audio_codec_ctx);
	}


	return NULL;
}

/**
 * mb_player_vdec_thread() -- This is the main decoding loop
 */
static void*
mb_player_stream_decode(void *arg)
{
	struct mbp *inst = (struct mbp*) arg;
	int i;
	AVPacket packet;


	MB_DEBUG_SET_THREAD_NAME("stream_decoder");


	assert(inst != NULL);
	assert(inst->media_file != NULL);
	assert(inst->window != NULL);
	assert(inst->status == MB_PLAYER_STATUS_PLAYING);
	assert(inst->fmt_ctx == NULL);
	assert(inst->audio_paused == 0);
	assert(inst->video_paused == 0);

	inst->have_audio = 0;
	inst->have_video = 0;

	/* get the size of the window */
	if (mbv_window_getsize(inst->window, &inst->width, &inst->height) == -1) {
		fprintf(stderr, "player: Could not get window size\n");
		goto decoder_exit;
	}


	fprintf(stderr, "player: Attempting to play (%ix%i) '%s'\n",
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

	inst->video_quit = 0;
	inst->video_decoder_quit = 0;
	inst->video_playback_index = 0;
	inst->video_decode_index = 0;
	inst->video_frames = 0;
	inst->video_packet_read_index = 0;
	inst->video_packet_write_index = 0;
	inst->video_packets = 0;

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

	if (av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) >= 0) {

		fprintf(stderr, "player: audio found\n");

		/* allocate filtered audio frames */
		inst->audio_pcm_handle = NULL;
		inst->audio_decode_index = 0;
		inst->audio_playback_index = 0;
		inst->audio_frames = 0;
		inst->audio_packets = 0;
		inst->audio_quit = 0;
		inst->audio_decoder_quit = 0;
		inst->audio_stream_index = -1;
		inst->audio_packet_write_index = 0;
		inst->audio_packet_read_index = 0;
		inst->audio_elapsed_before_trigger = 0;
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

	fprintf(stderr, "Stream decoder ready\n");
	pthread_mutex_lock(&inst->resume_lock);
	pthread_cond_signal(&inst->resume_signal);
	pthread_mutex_unlock(&inst->resume_lock);

	/* start decoding */
	while ((inst->action & MB_PLAYER_ACTION_STOP) == 0 && av_read_frame(inst->fmt_ctx, &packet) >= 0) {
		if (packet.stream_index == inst->video_stream_index) {
			/* if the buffer is full wait for the output thread to make room */
			while (inst->video_packet_state[inst->video_packet_write_index] == 1) {
				pthread_mutex_lock(&inst->video_decoder_lock);
				if (inst->action & MB_PLAYER_ACTION_STOP) {
					pthread_mutex_unlock(&inst->video_decoder_lock);
					goto decoder_exit;
				}
				if (inst->video_packet_state[inst->video_packet_write_index] == 1) {
					pthread_cond_wait(&inst->video_decoder_signal, &inst->video_decoder_lock);
					pthread_mutex_unlock(&inst->video_decoder_lock);
					if (inst->action & MB_PLAYER_ACTION_STOP) {
						goto decoder_exit;
					}
					continue;
				}
				/* unlocked bellow */
			}
			
			/* save the packet and signal decoder thread */
			inst->video_packet[inst->video_packet_write_index] = packet;
			inst->video_packet_state[inst->video_packet_write_index] = 1;
			pthread_mutex_unlock(&inst->video_decoder_lock);
			pthread_cond_signal(&inst->video_decoder_signal);
			pthread_cond_signal(&inst->video_output_signal);

			inst->video_packet_write_index++;
			inst->video_packet_write_index %= MB_VIDEO_BUFFER_PACKETS;

			//inst->video_packets++;
			__sync_fetch_and_add(&inst->video_packets, 1);

		} else if (packet.stream_index == inst->audio_stream_index) {
			while (inst->audio_packet_state[inst->audio_packet_write_index] == 1) {
				pthread_mutex_lock(&inst->audio_decoder_lock);
				if (inst->action & MB_PLAYER_ACTION_STOP) {
					pthread_mutex_unlock(&inst->audio_decoder_lock);
					goto decoder_exit;
				}
				if (inst->audio_packet_state[inst->audio_packet_write_index] == 1) {
					pthread_cond_wait(&inst->audio_decoder_signal, &inst->audio_decoder_lock);
					pthread_mutex_unlock(&inst->audio_decoder_lock);
					if (inst->action & MB_PLAYER_ACTION_STOP) {
						goto decoder_exit;
					}
					continue;
				}
				/* unlock below */
			}
			/* save the packet and signal decoder */
			inst->audio_packet[inst->audio_packet_write_index] = packet;
			inst->audio_packet_state[inst->audio_packet_write_index] = 1;
			pthread_mutex_unlock(&inst->audio_decoder_lock);
			pthread_cond_signal(&inst->audio_decoder_signal);
			pthread_cond_signal(&inst->audio_signal);

			inst->audio_packet_write_index++;
			inst->audio_packet_write_index %= MB_AUDIO_BUFFER_PACKETS;

			//inst->audio_packets++;
			__sync_fetch_and_add(&inst->audio_packets, 1);
		}
	}

decoder_exit:
	fprintf(stderr, "player: Stream decoder exiting\n");

	/* signal the renderer thread to exit and join it */
	pthread_mutex_lock(&inst->video_output_lock);
	inst->video_quit = 1;
	pthread_cond_signal(&inst->video_output_signal);
	pthread_mutex_unlock(&inst->video_output_lock);
	pthread_join(inst->video_output_thread, NULL);
	fprintf(stderr, "player: Video renderer exited\n");

	/* signal the video decoder thread to exit and join it */
	/* NOTE: Since this thread it's a midleman it waits on both locks */
	pthread_mutex_lock(&inst->video_decoder_lock);
	inst->video_decoder_quit = 1;
	pthread_cond_broadcast(&inst->video_decoder_signal);
	pthread_mutex_unlock(&inst->video_decoder_lock);
	pthread_mutex_lock(&inst->video_output_lock);
	pthread_cond_broadcast(&inst->video_output_signal);
	pthread_mutex_unlock(&inst->video_output_lock);

	pthread_join(inst->video_decoder_thread, NULL);
	fprintf(stderr, "player: Video decoder exited\n");

	/* clean audio stuff */
	if (inst->have_audio) {

		/* signal and wait for the audio thread to exit */
		pthread_mutex_lock(&inst->audio_lock);
		inst->audio_quit = 1;
		pthread_cond_signal(&inst->audio_signal);
		pthread_mutex_unlock(&inst->audio_lock);
		pthread_join(inst->audio_thread, NULL);
		fprintf(stderr, "player: Audio player exited\n");

		/* signal the audio decoder thread to exit and join it */
		/* NOTE: Since this thread it's a midleman it waits on both locks */
		pthread_mutex_lock(&inst->audio_decoder_lock);
		inst->audio_decoder_quit = 1;
		pthread_cond_broadcast(&inst->audio_decoder_signal);
		pthread_mutex_unlock(&inst->audio_decoder_lock);
		pthread_mutex_lock(&inst->audio_lock);
		pthread_cond_broadcast(&inst->audio_signal);
		pthread_mutex_unlock(&inst->audio_lock);
		pthread_join(inst->audio_decoder_thread, NULL);
		fprintf(stderr, "player: Audio decoder exited\n");


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
	inst->action = MB_PLAYER_ACTION_NONE;

	mb_player_updatestatus(inst, MB_PLAYER_STATUS_READY);

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


/**
 * mb_player_update() -- Redraw the media player window
 */
void
mb_player_update(struct mbp *inst)
{
	assert(inst != NULL);
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
 * mb_player_play() -- If path is not NULL it opens the file
 * specified by path and starts playing it. If path is NULL
 * it resumes playback if we're on the PAUSED state and return
 * failure code (-1) if we're on any other state.
 */
int 
mbp_play(struct mbp *inst, const char * const path)
{
	struct mbv_window *progress;

	assert(inst != NULL);
	assert(inst->status == MB_PLAYER_STATUS_READY ||
		inst->status == MB_PLAYER_STATUS_PLAYING ||
		inst->status == MB_PLAYER_STATUS_PAUSED);

	/* if no path argument was provided but we're already
	 * playing a file and we're paused then just resume
	 * playback */
	if (path == NULL) {
		if (inst->status == MB_PLAYER_STATUS_PAUSED) {
			pthread_mutex_lock(&inst->resume_lock);
			pthread_cond_broadcast(&inst->resume_signal);
			pthread_mutex_unlock(&inst->resume_lock);
			while (inst->audio_paused || inst->video_paused) {
				usleep(5000);
			}
			mb_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);
			return 0;
		}
		fprintf(stderr, "mbp_play() failed -- NULL path\n");
		return -1;
	}

	/* if we're already playing a file stop it first */
	if (inst->status != MB_PLAYER_STATUS_READY) {
		mbp_stop(inst);
	}

	/* initialize player object */
	inst->media_file = path;

	/* update status */
	mb_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);

	/* clear the screen */
	mbv_window_clear(inst->window, 0x00000000);
	mbv_window_update(inst->window);

	int sw, sh, pw, ph, px, py;
	mbv_window_getsize(inst->window, &sw, &sh);
	pw = (sw * 70) / 100;
	ph = 30;
	px = (sw / 2) - (pw / 2);
	py = (sh / 2) - (ph / 2);


	progress = mbv_window_new(NULL, px, py, pw, ph);
	assert(progress != NULL);

	mbv_window_show(progress);

	/* start the main decoder thread */
	pthread_mutex_lock(&inst->resume_lock);
	if (pthread_create(&inst->thread, NULL, mb_player_stream_decode, inst) != 0) {
		fprintf(stderr, "pthread_create() failed!\n");
		mb_player_updatestatus(inst, MB_PLAYER_STATUS_READY);
		return -1;
	}
	pthread_cond_wait(&inst->resume_signal, &inst->resume_lock);
	pthread_mutex_unlock(&inst->resume_lock);


	/* wait for the buffers to fill up */
	while (inst->audio_frames < MB_AUDIO_BUFFER_FRAMES &&
		inst->video_frames < MB_VIDEO_BUFFER_FRAMES) {

		/* update progressbar */
		int avail = inst->video_frames + inst->audio_frames;
		const int wanted = MB_AUDIO_BUFFER_FRAMES + MB_VIDEO_BUFFER_FRAMES;
		int pcent = (((avail * 100) / wanted) * 100) / 100;
		int donewidth = (pw * pcent) / 100;

		mbv_window_clear(progress, 0x3349ffFF);
		mbv_window_fillrectangle(progress, 0, 0, donewidth, ph);
		mbv_window_update(progress);

		mb_player_printstatus(inst, 0);
		usleep(5000);
	}

	mbv_window_destroy(progress);

	fprintf(stderr, "player: Firing rendering threads\n");

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
mbp_pause(struct mbp* inst)
{
	assert(inst != NULL);

	/* can't pause if we're not playing */
	if (inst->status != MB_PLAYER_STATUS_PLAYING) {
		fprintf(stderr, "player: Cannot puase, not playing\n");
		return -1;
	}

	/* request pause and wait for player thread to pause */
	inst->action |= MB_PLAYER_ACTION_PAUSE;

	/* wait for player to pause */
	if (inst->have_audio) {
		while (inst->audio_paused == 0) {
			usleep(5000);
		}
	} else {
		while (inst->video_paused == 0) {
			usleep(5000);
		}
	}

	inst->action &= ~MB_PLAYER_ACTION_PAUSE;

	mb_player_updatestatus(inst, MB_PLAYER_STATUS_PAUSED);
	return 0;
}


int
mbp_stop(struct mbp* inst)
{
	/* if the video is paused then unpause it first. */
	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		fprintf(stderr, "player: Unpausing stream\n");
		mbp_play(inst, NULL);
	}

	if (inst->status != MB_PLAYER_STATUS_READY) {
		fprintf(stderr, "player: Sending STOP action\n");
		inst->action |= MB_PLAYER_ACTION_STOP;

		pthread_cond_broadcast(&inst->audio_decoder_signal);
		pthread_cond_broadcast(&inst->video_decoder_signal);

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

	fprintf(stderr, "player: Initializing /dev/fb0\n");

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
			fprintf(stderr, "player: ioctl() failed\n");
			inst->use_fbdev = 0;
			goto end;
		}

		/* dump some screen info */
		fprintf(stderr, "player: bpp=%i\n", vinfo.bits_per_pixel);
		fprintf(stderr, "player: type=%i\n", finfo.type);
		fprintf(stderr, "player: visual=%i\n", finfo.visual);
		fprintf(stderr, "player: FOURCC (grayscale): '%c%c%c%c'\n",
			((char*)&vinfo.grayscale)[0], ((char*)&vinfo.grayscale)[1],
			((char*)&vinfo.grayscale)[2], ((char*)&vinfo.grayscale)[3]);
		fprintf(stderr, "player: xoffset=%i yoffset=%i r=%i g=%i b=%i\n"
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
		initialized = 1;
	}

	/* allocate memory for the player object */
	inst = malloc(sizeof(struct mbp));
	if (inst == NULL) {
		fprintf(stderr, "mbp_init() failed -- out of memory\n");
		return NULL;
	}

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
	inst->media_file = NULL;
	inst->buf = NULL;
	inst->fmt_ctx = NULL;
	inst->bufsz = 0;
	inst->use_fbdev = 1;
	inst->video_stream_index = -1;
	inst->video_paused = 0;
	inst->audio_paused = 0;
	inst->status_callback = NULL;
	inst->action = MB_PLAYER_ACTION_NONE;
	inst->status = MB_PLAYER_STATUS_READY;

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
		pthread_cond_init(&inst->resume_signal, NULL) != 0) {
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

	fprintf(stderr, "player: Destroying\n");

	/* this just fails if we're not playing */
	(void) mbp_stop(inst);

	if (inst->media_file != NULL) {
		free(inst);
	}
}

