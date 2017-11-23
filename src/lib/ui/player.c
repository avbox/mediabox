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


#ifdef HAVE_CONFIG_H
#	include "../../config.h"
#endif

#define HAVE_MALLOC_TRIM	(1)	/* TODO: Check for this on configure */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
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
#ifdef ENABLE_DVD
#	include <dvdnav/dvdnav.h>
#endif
#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#define LOG_MODULE "player"

#include "video.h"
#include "player.h"
#include "../debug.h"
#include "../log.h"
#include "../su.h"
#include "../time_util.h"
#include "../timers.h"
#include "../linkedlist.h"
#include "../audio.h"
#include "../compiler.h"
#include "../queue.h"
#include "../dispatch.h"
#include "../application.h"
#include "../math_util.h"
#include "../ffmpeg_util.h"
#include "../checkpoint.h"
#include "../thread.h"
#include "../stopwatch.h"

#ifdef ENABLE_DVD
#	include "../dvdio.h"
#	include "input.h"
#endif


/* This is the # of frames to decode ahead of time */
#define MB_VIDEO_BUFFER_PACKETS (1)
#define MB_AUDIO_BUFFER_PACKETS (1)

#define AVBOX_BUFFER_MSECS	(200)
#define AVBOX_BUFFER_VIDEO	(30 / (1000 / AVBOX_BUFFER_MSECS))
#define AVBOX_BUFFER_AUDIO	(48000 / (1000 / AVBOX_BUFFER_MSECS))

#define ALIGNED(addr, bytes) \
    (((uintptr_t)(const void *)(addr)) % (bytes) == 0)


#define AVBOX_PLAYERCTL_PLAY				(0x01)
#define AVBOX_PLAYERCTL_PAUSE				(0x02)
#define AVBOX_PLAYERCTL_STOP				(0x03)
#define AVBOX_PLAYERCTL_SEEK				(0x04)
#define AVBOX_PLAYERCTL_THREADEXIT			(0x05)
#define AVBOX_PLAYERCTL_STREAM_READY			(0x06)
#define AVBOX_PLAYERCTL_AUDIODEC_READY			(0x07)
#define AVBOX_PLAYERCTL_VIDEODEC_READY			(0x08)
#define AVBOX_PLAYERCTL_AUDIOOUT_READY			(0x09)
#define AVBOX_PLAYERCTL_VIDEOOUT_READY 			(0x0A)
#define AVBOX_PLAYERCTL_STREAM_EXIT			(0x0B)
#define AVBOX_PLAYERCTL_BUFFER_UNDERRUN			(0x0C)
#define AVBOX_PLAYERCTL_AUDIO_STREAM_DRIED		(0x0D)


#ifdef ENABLE_DVD
#define AVBOX_PLAYERCTL_DVD				(0x0700)
#define AVBOX_PLAYERCTL_DVD_VTS_CHANGE			(AVBOX_PLAYERCTL_DVD | DVDNAV_VTS_CHANGE)
#define AVBOX_PLAYERCTL_DVD_WAIT			(AVBOX_PLAYERCTL_DVD | DVDNAV_WAIT)
#define AVBOX_PLAYERCTL_DVD_STILL_FRAME			(AVBOX_PLAYERCTL_DVD | DVDNAV_STILL_FRAME)
#define AVBOX_PLAYERCTL_DVD_AUDIO_STREAM_CHANGE		(AVBOX_PLAYERCTL_DVD | DVDNAV_AUDIO_STREAM_CHANGE)
#define AVBOX_PLAYERCTL_DVD_HOP_CHANNEL			(AVBOX_PLAYERCTL_DVD | DVDNAV_HOP_CHANNEL)
#define AVBOX_PLAYERCTL_DVD_HIGHLIGHT			(AVBOX_PLAYERCTL_DVD | DVDNAV_HIGHLIGHT)
#define AVBOX_PLAYERCTL_DVD_NAV_PACKET			(AVBOX_PLAYERCTL_DVD | DVDNAV_NAV_PACKET)
#define AVBOX_PLAYERCTL_DVD_CELL_CHANGE			(AVBOX_PLAYERCTL_DVD | DVDNAV_CELL_CHANGE)
#define AVBOX_PLAYERCTL_DVD_SPU_CLUT_CHANGE		(AVBOX_PLAYERCTL_DVD | DVDNAV_SPU_CLUT_CHANGE)
#define AVBOX_PLAYERCTL_DVD_SPU_STREAM_CHANGE		(AVBOX_PLAYERCTL_DVD | DVDNAV_SPU_STREAM_CHANGE)
#endif


/* playback startup stages */
#define AVBOX_PLAYER_PLAYSTATE_READY		(0x00)
#define AVBOX_PLAYER_PLAYSTATE_STREAM		(0x01)
#define AVBOX_PLAYER_PLAYSTATE_AUDIODEC		(0x02)
#define AVBOX_PLAYER_PLAYSTATE_VIDEODEC		(0x03)
#define AVBOX_PLAYER_PLAYSTATE_AUDIOOUT		(0x04)
#define AVBOX_PLAYER_PLAYSTATE_VIDEOOUT		(0x05)
#define AVBOX_PLAYER_PLAYSTATE_PLAYING		(0x06)
#define AVBOX_PLAYER_PLAYSTATE_STOPPING		(0x07)


enum avbox_aspect_ratio
{
	AVBOX_ASPECT_16_9 = 0,
	AVBOX_ASPECT_4_3 = 1
};


struct avbox_size
{
	int w;
	int h;
};


LISTABLE_STRUCT(avbox_player_subscriber,
	struct avbox_object *object;
);


struct avbox_player_ctlmsg
{
	int id;
	void *data;
};


struct avbox_player_seekargs
{
	int64_t pos;
	int flags;
};


struct avbox_player_waiter
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	void *data;
};


struct avbox_player_state_info
{
	int64_t pos;
	int64_t duration;
	char *title;
};


struct avbox_audio_decoder_args
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct avbox_player *inst;
};


/**
 * Player structure.
 */
struct avbox_player
{
	struct avbox_player_state_info state_info;
	struct avbox_window *window;
	struct avbox_window *video_window;
	struct avbox_object *object;
	struct avbox_object *control_object;
	struct avbox_queue *video_packets_q;
	struct avbox_queue *audio_packets_q;
	struct avbox_queue *video_frames_q;
	struct avbox_audiostream *audio_stream;

#ifdef ENABLE_DVD
	struct avbox_dvdio *dvdio;
	struct avbox_rect highlight;
	int logical_audio_stream;
	int flushing;
#endif
	struct avbox_rational aspect_ratio;
	struct avbox_size video_size;
	struct timespec systemreftime;
	enum avbox_player_status status;

	AVFormatContext *fmt_ctx;
	AVCodecContext *audio_codec_ctx;
	AVCodecContext *video_codec_ctx;
	AVPacket packet;

	const char *media_file;
	const char *next_file;

	int underrun_timer_id;
	int stream_exit_timer_id;
	int audio_stream_index;
	int video_stream_index;
	int play_state;
	int stream_quit;
	int gotpacket;
	int stream_percent;
	int stream_exiting;
	int video_decoder_flushed;
	int audio_decoder_flushed;

	/* i don't think these are needed anymore */
	int audio_time_set;

	struct avbox_stopwatch *video_time;
	int64_t (*getmastertime)(struct avbox_player *inst);

	avbox_checkpoint_t video_decoder_checkpoint;
	avbox_checkpoint_t video_output_checkpoint;
	avbox_checkpoint_t audio_decoder_checkpoint;
	avbox_checkpoint_t stream_parser_checkpoint;
	pthread_t video_decoder_thread;
	pthread_t video_output_thread;
	struct avbox_delegate *audio_decoder_worker;
	pthread_t control_thread;
	pthread_mutex_t state_lock;
	pthread_t stream_thread;
	LIST subscribers;

	/* playlist stuff */
	LIST playlist;
	struct avbox_playlist_item *playlist_item;
};


/**
 * Sends a control message to the player.
 */
static void
avbox_player_sendctl(struct avbox_player * const inst,
	const int ctl, void * const data)
{
	struct avbox_player_ctlmsg * msg;
	if ((msg = malloc(sizeof(struct avbox_player_ctlmsg))) == NULL) {
		LOG_PRINT_ERROR("Could not send control message: Out of memory");
		return;
	}

	msg->id = ctl;
	msg->data = data;

	if (avbox_object_sendmsg(&inst->control_object,
		AVBOX_MESSAGETYPE_USER, AVBOX_DISPATCH_UNICAST, msg) == NULL) {
		LOG_VPRINT_ERROR("Could not send control message: %s",
			strerror(errno));
		free(msg);
	}
}


/**
 * Calculate the resolution to scale to with aspect
 * ratio adjustment.
 */
static void
avbox_player_scale2display(
	struct avbox_player * const inst,
	int w, int h,
	struct avbox_size *out)
{
	struct avbox_size screen;
	struct avbox_size in;
	screen.w = w;
	screen.h = h;
	in.w = inst->video_codec_ctx->width;
	in.h = inst->video_codec_ctx->height;

	ASSERT(screen.w >= screen.h);

#define SCALE (10000)
	if (in.w > in.h) {
bw:
		/* first scale to fit to resolution and then
		 * adjust to aspect ratio */
		out->w = screen.w * SCALE;
		out->h = (((in.h * SCALE) * ((out->w * 100) / (in.w * SCALE))) / 100);
		out->h += (out->h * ((((screen.h * SCALE) - (((screen.w * SCALE) * inst->aspect_ratio.den)
			/ inst->aspect_ratio.num)) * 100) / (screen.h * SCALE))) / 100;
		if ((out->h / SCALE) > screen.h) {
			goto bh;
		}
	} else {
bh:
		/* first scale to fit to resolution and then
		 * adjust to aspect ratio */
		out->h = screen.h * SCALE;
		#define INW (in.w * SCALE)
		#define INH (in.h * SCALE)
		out->w = ((INW * ((out->h * 100) / INH)) / 100);

		#define SW (screen.w * SCALE)
		#define SH (screen.h * SCALE)

		const int tmp = (((SW - ((SH * inst->aspect_ratio.num)
			/ inst->aspect_ratio.den)) * 100) / SW);

		out->w += (out->w * tmp) / 100;

		/* for now */
		if (out->w < SW) {
			out->w = SW;
		}

		if ((out->w / SCALE) > screen.w) {
			goto bw;
		}

		#undef SW
		#undef SH
		#undef INW
		#undef INH
	}

	/* scale result */
	out->w /= SCALE;
	out->h /= SCALE;

	ASSERT(out->w <= screen.w);
	ASSERT(out->h <= screen.h);
#undef SCALE
}


/**
 * Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
static int64_t
avbox_player_getaudiotime(struct avbox_player * const inst)
{
	ASSERT(inst->audio_stream != NULL);
	return avbox_audiostream_gettime(inst->audio_stream);
}


static int64_t
avbox_player_getsystemtime(struct avbox_player *inst)
{
	ASSERT(inst->video_time != NULL);
	return avbox_stopwatch_time(inst->video_time);
}


/**
 * Draw handler for the target window.
 */
static int
avbox_player_draw(struct avbox_window * const window, void * const context)
{
	struct avbox_player * const inst = context;
	avbox_window_blit(window, inst->video_window, MBV_BLITFLAGS_NONE, 0, 0);

#if ENABLE_DVD
	if (inst->highlight.x != 0 || inst->highlight.y != 0) {
		uint32_t width, height;
		int target_width, target_height;

		/* for now we need to scale here. once the video scaling is moved
		 * to the video driver we won't need this */

		if (dvdnav_get_video_resolution(avbox_dvdio_dvdnav(inst->dvdio), &width, &height) != 0) {
			LOG_PRINT_ERROR("Could not get VTS resolution!");
		}

		avbox_window_getcanvassize(inst->window, &target_width, &target_height);

		const int wpc = (100 * inst->video_size.w) / width;
		const int hpc = (100 * inst->video_size.h) / height;
		const int wos = (target_width > inst->video_size.w) ? (target_width - inst->video_size.w) / 2 : 0;
		const int hos = (target_height > inst->video_size.h) ? (target_height - inst->video_size.h) / 2 : 0;

		avbox_window_fillrectangle(inst->window,
			((inst->highlight.x * wpc) / 100) + wos, ((inst->highlight.y * hpc) / 100) + hos,
			((inst->highlight.w * wpc) / 100), ((inst->highlight.h * hpc) / 100));
	}
#endif
	return 0;
}


/**
 * Update the display from main thread.
 */
static void *
avbox_player_doupdate(void *arg)
{
	struct avbox_player * const inst = (struct avbox_player*) arg;
	avbox_window_update(inst->window);
	return NULL;
}


/**
 * Video rendering thread.
 */
static void *
avbox_player_video(void *arg)
{
	int pitch, linesize, height, target_width, target_height;
	uint8_t *buf;
	struct avbox_player *inst = (struct avbox_player*) arg;
	AVFrame *frame;
	struct avbox_delegate *del;
	struct SwsContext *swscale_ctx = NULL;

	DEBUG_SET_THREAD_NAME("video_playback");
	DEBUG_PRINT("player", "Video renderer started");

	ASSERT(inst != NULL);
	ASSERT(inst->video_window == NULL);

	linesize = av_image_get_linesize(MB_DECODER_PIX_FMT, inst->video_codec_ctx->width, 0);
	height = inst->video_codec_ctx->height;

	/* get the size of the target window */
	avbox_window_getcanvassize(inst->window, &target_width, &target_height);

	/* create an offscreen window for rendering */
	if ((inst->video_window = avbox_window_new(NULL, "video_surface", 0,
		0, 0, target_width, target_height,
		NULL, NULL, NULL)) == NULL) {
		LOG_PRINT_ERROR("Could not create video window!");
		goto video_exit;
	}

	/* clear the off-screen window and set a draw handler
	 * for the target window */
	avbox_window_setbgcolor(inst->video_window, AVBOX_COLOR(0x000000ff));
	avbox_window_clear(inst->video_window);
	avbox_window_setdrawfunc(inst->window, avbox_player_draw, inst);

	/* calculate how to scale the video */
	avbox_player_scale2display(inst,
		target_width, target_height, &inst->video_size);

	/* initialize the software scaler */
	if ((swscale_ctx = sws_getContext(
		inst->video_codec_ctx->width,
		inst->video_codec_ctx->height,
		MB_DECODER_PIX_FMT,
		inst->video_size.w,
		inst->video_size.h,
		MB_DECODER_PIX_FMT,
		SWS_PRINT_INFO | SWS_FAST_BILINEAR,
		NULL, NULL, NULL)) == NULL) {
		LOG_PRINT_ERROR("Could not create swscale context!");
		avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT,
			&inst->video_output_thread);
		goto video_exit;
	}

	avbox_checkpoint_enable(&inst->video_output_checkpoint);

	/* signal control thread that we're ready */
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_VIDEOOUT_READY, NULL);

	DEBUG_PRINT("player", "Video renderer ready");

	int underrun = 1;

	while (1) {

		avbox_checkpoint_here(&inst->video_output_checkpoint);

		if (!underrun && (frame = avbox_queue_timedpeek(inst->video_frames_q, 500L * 1000L)) == NULL) {
			if (errno == EAGAIN) {
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_BUFFER_UNDERRUN, NULL);
				underrun = 1;
				continue;
			} else if (errno == ESHUTDOWN) {
				break;
			} else {
				LOG_VPRINT_ERROR("Error!: avbox_queue_timedpeek() failed: %s",
					strerror(errno));
				goto video_exit;
			}
		} else {
			/* get the next decoded frame */
			if ((frame = avbox_queue_peek(inst->video_frames_q, 1)) == NULL) {
				if (errno == EAGAIN) {
					continue;
				} else if (errno == ESHUTDOWN) {
					break;
				} else {
					LOG_VPRINT_ERROR("Error!: avbox_queue_get() failed: %s",
						strerror(errno));
					goto video_exit;
				}
			}
			underrun = 0;
		}

		/* get the frame pts and wait */
		if  (LIKELY(frame->pts != AV_NOPTS_VALUE)) {
			const int64_t current_time = inst->getmastertime(inst);
			const int64_t frame_time = av_rescale_q(av_frame_get_best_effort_timestamp(frame),
				inst->fmt_ctx->streams[inst->video_stream_index]->time_base,
				AV_TIME_BASE_Q);

			/* NOTE: frame_time may be a very large negative interger that
			 * can overflow so we need to check that current_time < frame_time
			 * before substracting */
			if (current_time < frame_time) {
				const int64_t delay = frame_time - current_time;
				if (LIKELY((delay & ~0xFF) > 0)) {
					/* don't block for too long */
					if (delay > 250L * 1000L) {
						usleep(250L * 1000L);
					} else {
						usleep(delay);
					}
					continue;
				}
			}
		}

		/* copy the frame to the video window. For now we
		 * just scale here but in the future this should be done
		 * by the video driver (possibly accelerated). */
		if ((buf = avbox_window_lock(inst->video_window, MBV_LOCKFLAGS_WRITE, &pitch)) == NULL) {
			LOG_VPRINT_ERROR("Could not lock video window: %s", strerror(errno));
		} else {
			ASSERT(ALIGNED(*frame->data, 16));
			ASSERT(ALIGNED(buf, 16));
			buf += pitch * ((target_height - inst->video_size.h) / 2);
			buf += (pitch / target_width) * ((target_width - inst->video_size.w) / 2);
			sws_scale(swscale_ctx, (uint8_t const * const *) frame->data,
				&linesize, 0, height, &buf, &pitch);
			avbox_window_unlock(inst->video_window);
		}

		/* perform the actual update from the main thread */
		if ((del = avbox_application_delegate(avbox_player_doupdate, inst)) == NULL) {
			LOG_PRINT_ERROR("Could not delegate update!");
		} else {
			avbox_delegate_wait(del, NULL);
		}

		/* update buffer state and signal decoder */
		if (avbox_queue_get(inst->video_frames_q) != frame) {
			LOG_PRINT_ERROR("We peeked one frame but got another one!");
			abort();
		}

		av_frame_unref(frame);
		av_free(frame);
	}

video_exit:
	DEBUG_PRINT("player", "Video renderer exiting");

	avbox_checkpoint_disable(&inst->video_output_checkpoint);

	if (swscale_ctx != NULL) {
		sws_freeContext(swscale_ctx);
	}

	/* free any frames left in the queue */
	while ((frame = avbox_queue_get(inst->video_frames_q)) != NULL) {
		av_frame_unref(frame);
		av_free(frame);
	}

	/* clear screen */
	avbox_window_clear(inst->video_window);
	if ((del = avbox_application_delegate(avbox_player_doupdate, inst)) == NULL) {
		LOG_PRINT_ERROR("Could not delegate update!");
	} else {
		avbox_delegate_wait(del, NULL);
	}

	avbox_window_setdrawfunc(inst->window, NULL, NULL);

	return NULL;
}


/**
 * Decodes video frames in the background.
 */
static void *
avbox_player_video_decode(void *arg)
{
	int ret, flushed, just_flushed = 0, keep_going;
	struct avbox_player *inst = (struct avbox_player*) arg;
	char video_filters[512];
	AVPacket *packet = NULL;
	AVFrame *video_frame_nat = NULL, *video_frame_flt = NULL;
	AVFilterGraph *video_filter_graph = NULL;
	AVFilterContext *video_buffersink_ctx = NULL;
	AVFilterContext *video_buffersrc_ctx = NULL;

	DEBUG_SET_THREAD_NAME("video_decode");
	DEBUG_PRINT("player", "Video decoder starting");

	ASSERT(inst != NULL);
	ASSERT(inst->fmt_ctx != NULL);
	ASSERT(inst->video_codec_ctx == NULL);
	ASSERT(inst->video_stream_index != -1);

	/* open the video codec */
	if ((inst->video_codec_ctx = avbox_ffmpegutil_opencodeccontext(
		&inst->video_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_VIDEO)) == NULL) {
		LOG_PRINT_ERROR("Could not open video codec context");
		goto decoder_exit;
	}

	/* initialize video filter graph */
	strcpy(video_filters, "null");
	DEBUG_VPRINT("player", "Video width: %i height: %i",
		inst->video_codec_ctx->width, inst->video_codec_ctx->height);
	DEBUG_VPRINT("player", "Video filters: %s", video_filters);
	if (avbox_ffmpegutil_initvideofilters(inst->fmt_ctx, inst->video_codec_ctx,
		&video_buffersink_ctx, &video_buffersrc_ctx, &video_filter_graph,
		video_filters, inst->video_stream_index) < 0) {
		LOG_PRINT_ERROR("Could not initialize filtergraph!");
		goto decoder_exit;
	}

	/* allocate video frames */
	video_frame_nat = av_frame_alloc(); /* native */
	if (video_frame_nat == NULL) {
		LOG_PRINT_ERROR("Could not allocate frames!");
		goto decoder_exit;
	}

	avbox_checkpoint_enable(&inst->video_decoder_checkpoint);

	/* signal control trhead that we're ready */
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_VIDEODEC_READY, NULL);

	DEBUG_PRINT("player", "Video decoder ready");


	for (inst->video_decoder_flushed = flushed = 1; 1;) {

		avbox_checkpoint_here(&inst->video_decoder_checkpoint);

		/* get next packet from queue */
		if (!flushed && (packet = avbox_queue_timedpeek(inst->video_packets_q, 500L * 1000L)) == NULL) {
			if (errno == EAGAIN || errno == ESHUTDOWN) {
				DEBUG_PRINT(LOG_MODULE, "Sending flush packet to video decoder");
				ret = avcodec_send_packet(inst->video_codec_ctx, NULL);
				flushed = just_flushed = 1;
				/* fall through */
			} else {
				LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
					strerror(errno));
				break;
			}
		} else {
			if (packet == NULL && (packet = avbox_queue_peek(inst->video_packets_q, 1)) == NULL) {
				if (errno == EAGAIN) {
					continue;
				} else if (errno == ESHUTDOWN) {
					break;
				} else {
					LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
						strerror(errno));
					break;
				}
			} else {
				/* send packet to codec for decoding */
				if (UNLIKELY((ret  = avcodec_send_packet(inst->video_codec_ctx, packet)) < 0)) {
					if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
						/* fall through */
						ret = 0;

					} else if (ret == AVERROR_INVALIDDATA) {
						LOG_PRINT_ERROR("Invalid data sent to video decoder");
						ret = 0; /* so we still dequeue it */

					} else {
						char err[256];
						av_strerror(ret, err, sizeof(err));
						LOG_VPRINT_ERROR("Error decoding video packet (%i): %s",
							ret, err);
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
						goto decoder_exit;
					}
				}
				inst->video_decoder_flushed = flushed = 0;
			}
		}

		/* read decoded frames from codec */
		for (keep_going = 1; keep_going;) {

			/* grab the next frame and add it to the filtergraph */
			if (LIKELY((ret = avcodec_receive_frame(inst->video_codec_ctx, video_frame_nat))) == 0) {
				if (video_frame_nat->pkt_dts == AV_NOPTS_VALUE) {
					video_frame_nat->pts = 0;
				} else {
					video_frame_nat->pts = video_frame_nat->pkt_dts;
				}

				/* push the decoded frame into the filtergraph */
				if (UNLIKELY(av_buffersrc_add_frame_flags(video_buffersrc_ctx,
					video_frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF |
					AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT) < 0)) {
					LOG_PRINT_ERROR("Error feeding video filtergraph");
					goto decoder_exit;
				}
			} else {
				if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
					LOG_VPRINT_ERROR("ERROR: avcodec_receive_frame() returned %d (video)",
						ret);
				}
				keep_going = 0;
			}

			/* pull filtered frames from the filtergraph */
			while (1) {
				if ((video_frame_flt = av_frame_alloc()) == NULL) {
					LOG_PRINT_ERROR("Cannot allocate AVFrame: Out of memory!");
					continue;
				}

				ret = av_buffersink_get_frame(video_buffersink_ctx, video_frame_flt);
				if (UNLIKELY(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)) {
					av_free(video_frame_flt);
					video_frame_flt = NULL;
					break;
				}
				if (UNLIKELY(ret < 0)) {
					LOG_VPRINT_ERROR("Could not get video frame from filtergraph (ret=%i)",
						ret);
					av_free(video_frame_flt);
					video_frame_flt = NULL;
					goto decoder_exit;
				}

				ASSERT(video_buffersink_ctx->inputs[0]->time_base.num == inst->fmt_ctx->streams[inst->video_stream_index]->time_base.num);
				ASSERT(video_buffersink_ctx->inputs[0]->time_base.den == inst->fmt_ctx->streams[inst->video_stream_index]->time_base.den);

				/* add frame to decoded frames queue */
				while (1) {
					if (avbox_queue_put(inst->video_frames_q, video_frame_flt) == -1) {
						if (errno == EAGAIN) {
							continue;
						} else if (errno == ESHUTDOWN) {
							LOG_PRINT_ERROR("Video frames queue closed unexpectedly!");
						} else {
							LOG_VPRINT_ERROR("Error: avbox_queue_put() failed: %s",
								strerror(errno));
						}
						av_frame_unref(video_frame_flt);
						av_free(video_frame_flt);
						video_frame_flt = NULL;
						goto decoder_exit;
					}
					break;
				}

				video_frame_flt = NULL;
			}
			av_frame_unref(video_frame_nat);
		}

		/* dequeue and free video packet */
		if (packet != NULL) {
			if (avbox_queue_get(inst->video_packets_q) != packet) {
				LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result: %s",
					strerror(errno));
				goto decoder_exit;
			}
			av_packet_unref(packet);
			free(packet);
			packet = NULL;
		}

		/* if we just flushed the codec pipeline flush the
		 * buffers so we can keep using it */
		if (just_flushed) {
			DEBUG_PRINT(LOG_MODULE, "Flushing video codec");
			avcodec_flush_buffers(inst->video_codec_ctx);
			inst->video_decoder_flushed = 1;
			just_flushed = 0;
		}
	}
decoder_exit:
	DEBUG_PRINT("player", "Video decoder exiting");

	avbox_checkpoint_disable(&inst->video_decoder_checkpoint);

	/* signal the video thread to exit and join it */
	if (inst->video_frames_q != NULL) {
		avbox_queue_close(inst->video_frames_q);
	}

	ASSERT(video_frame_flt == NULL);

	if (video_buffersink_ctx != NULL) {
		DEBUG_PRINT("player", "Flushing video filter graph");
		if ((video_frame_flt = av_frame_alloc()) != NULL) {
			while ((ret = av_buffersink_get_frame(video_buffersink_ctx, video_frame_flt)) >= 0) {
				av_frame_unref(video_frame_flt);
			}
			if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
				char err[256];
				av_strerror(ret, err, sizeof(err));
				LOG_VPRINT_ERROR("Could not flush video filter graph: %s", err);
			}
			av_free(video_frame_flt);
		} else {
			LOG_PRINT_ERROR("LEAK: Could not flush filter graph!");
		}
		avfilter_free(video_buffersink_ctx);
	}
	if (video_buffersrc_ctx != NULL) {
		avfilter_free(video_buffersrc_ctx);
	}
	if (video_filter_graph != NULL) {
		avfilter_graph_free(&video_filter_graph);
	}
	if (inst->video_codec_ctx != NULL) {
		DEBUG_PRINT("player", "Flushing video decoder");
		while (avcodec_receive_frame(inst->video_codec_ctx, video_frame_nat) == 0) {
			av_frame_unref(video_frame_nat);
		}
		avcodec_flush_buffers(inst->video_codec_ctx);
		/* TODO: Close codec here */
	}
	if (video_frame_nat != NULL) {
		av_free(video_frame_nat);
	}

	DEBUG_PRINT("player", "Video decoder bailing out");

	return NULL;
}


/**
 * Decodes the audio stream.
 */
static void *
avbox_player_audio_decode(void * arg)
{
	int ret, keep_going, flushed, just_flushed = 0;
	struct avbox_audio_decoder_args *args = arg;
	struct avbox_player * const inst = args->inst;
	const char *audio_filters ="aresample=48000,aformat=sample_fmts=s16:channel_layouts=stereo";
	AVFrame *audio_frame_nat = NULL;
	AVFrame *audio_frame = NULL;
	AVFilterGraph *audio_filter_graph = NULL;
	AVFilterContext *audio_buffersink_ctx = NULL;
	AVFilterContext *audio_buffersrc_ctx;
	AVPacket *packet = NULL;

	DEBUG_SET_THREAD_NAME("audio_decoder");

	ASSERT(inst != NULL);
	ASSERT(inst->fmt_ctx != NULL);
	ASSERT(inst->audio_codec_ctx == NULL);
	ASSERT(inst->audio_packets_q != NULL);
	ASSERT(inst->audio_stream_index != -1);

	DEBUG_PRINT("player", "Audio decoder starting");

	/* open the audio codec */
	if ((inst->audio_codec_ctx = avbox_ffmpegutil_opencodeccontext(
		&inst->audio_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_AUDIO)) == NULL) {
		LOG_PRINT_ERROR("Could not open audio codec!");
		goto end;
	}

	/* allocate audio frames */
	audio_frame_nat = av_frame_alloc();
	audio_frame = av_frame_alloc();
	if (audio_frame_nat == NULL || audio_frame == NULL) {
		LOG_PRINT_ERROR("Could not allocate audio frames");
		goto end;
	}

	/* initialize audio filter graph */
	DEBUG_VPRINT("player", "Audio filters: %s", audio_filters);
	if (avbox_ffmpegutil_initaudiofilters(inst->fmt_ctx, inst->audio_codec_ctx,
		&audio_buffersink_ctx, &audio_buffersrc_ctx, &audio_filter_graph,
		audio_filters, inst->audio_stream_index,
#ifdef ENABLE_DVD
		(inst->dvdio != NULL) ? avbox_dvdio_dvdnav(inst->dvdio) : NULL
#else
		NULL
#endif
		) < 0) {
		LOG_PRINT_ERROR("Could not init filter graph!");
		goto end;
	}

	avbox_checkpoint_enable(&inst->audio_decoder_checkpoint);

	pthread_mutex_lock(&args->mutex);
	pthread_cond_signal(&args->cond);
	pthread_mutex_unlock(&args->mutex);

	/* signl control thread that we're ready */
	if (inst->play_state == AVBOX_PLAYER_PLAYSTATE_AUDIODEC) {
		avbox_player_sendctl(inst, AVBOX_PLAYERCTL_AUDIODEC_READY, NULL);
	}

	DEBUG_PRINT("player", "Audio decoder ready");

	for (inst->audio_decoder_flushed = flushed = 1; 1;) {

		avbox_checkpoint_here(&inst->audio_decoder_checkpoint);

		if (!flushed &&
			(packet = avbox_queue_timedpeek(inst->audio_packets_q, 500L * 1000L)) == NULL) {
			if (errno == EAGAIN || errno == ESHUTDOWN) {
				DEBUG_PRINT(LOG_MODULE, "Sending audio flush packet");
				ret = avcodec_send_packet(inst->audio_codec_ctx, NULL);
				flushed = just_flushed = 1;
				/* fall through */
			} else {
				LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
					strerror(errno));
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
				goto end;
			}
		} else {
			/* wait for the stream decoder to give us some packets */
			if (packet == NULL && (packet = avbox_queue_peek(inst->audio_packets_q, 1)) == NULL) {
				if (errno == EAGAIN) {
					packet = NULL;
					continue;

				} else if (errno == ESHUTDOWN) {
					break;

				} else {
					LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
						strerror(errno));
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
					goto end;
				}
			} else {
				/* send packets to codec for decoding */
				if ((ret = avcodec_send_packet(inst->audio_codec_ctx, packet)) < 0) {
					if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
						packet = NULL;
						/* fall through */

					} else if (ret == AVERROR(EINVAL) || ret == AVERROR_INVALIDDATA) {
						ret = 0; /* so we still dequeue it */

					} else if (ret == AVERROR(ENOMEM)) {
						LOG_PRINT_ERROR("Audio decoder out of memory");
						abort();

					} else {
						char err[256];
						av_strerror(ret, err, sizeof(err));
						LOG_VPRINT_ERROR("Error decoding audio(%i): %s",
							ret, err);
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT,
							&inst->audio_decoder_worker);
						goto end;
					}
				}
				if (ret == 0) {
					/* remove packet from queue */
					if (avbox_queue_get(inst->audio_packets_q) != packet) {
						LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result (%p): %s",
							packet, strerror(errno));
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT,
							&inst->audio_decoder_worker);
						goto end;
					}
					/* free packet */
					av_packet_unref(packet);
					free(packet);
					packet = NULL;
				}
				inst->audio_decoder_flushed = flushed = 0;
			}
		}

		for (keep_going = 1; keep_going;) {

			/* get the next frame from the decoder */
			if ((ret = avcodec_receive_frame(inst->audio_codec_ctx, audio_frame_nat)) != 0) {
				if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
					LOG_VPRINT_ERROR("ERROR!: avcodec_receive_frame() returned %d (audio)",
						AVERROR(ret));
				}
				keep_going = 0;
			} else {
				/* push the audio data from decoded frame into the filtergraph */
				if (UNLIKELY((ret = av_buffersrc_add_frame_flags(
					audio_buffersrc_ctx, audio_frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0)) {
					char err[256];
					av_strerror(ret, err, sizeof(err));
					LOG_VPRINT_ERROR("Error while feeding the audio filtergraph: %s (channels=%i|layout=0x%"PRIx64")",
						err, audio_frame_nat->channels, audio_frame_nat->channel_layout);
					av_frame_unref(audio_frame_nat);
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT,
						&inst->audio_decoder_worker);
				}
				keep_going = 1;
			}

			/* pull filtered audio from the filtergraph */
			while (1) {

				ret = av_buffersink_get_frame(audio_buffersink_ctx, audio_frame);
				if (UNLIKELY(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)) {
					av_frame_unref(audio_frame);
					break;
				}
				if (UNLIKELY(ret < 0)) {
					LOG_PRINT_ERROR("Error reading from buffersink");
					av_frame_unref(audio_frame);
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT,
						&inst->audio_decoder_worker);

					goto end;
				}

				/* if this is the first frame then set the audio stream
				 * clock to it's pts. This is needed because not all streams
				 * start at pts 0 */
				if (UNLIKELY(!inst->audio_time_set)) {
					int64_t pts;
					pts = av_frame_get_best_effort_timestamp(audio_frame);
					pts = av_rescale_q(pts,
						inst->fmt_ctx->streams[inst->audio_stream_index]->time_base,
						AV_TIME_BASE_Q);
					avbox_audiostream_setclock(inst->audio_stream, pts);
					inst->getmastertime = avbox_player_getaudiotime;
					DEBUG_VPRINT("player", "First audio pts: %li unscaled=%li",
						pts, audio_frame->pts);
					inst->audio_time_set = 1;
				}

				/* write frame to audio stream and free it */
				while (avbox_audiostream_write(inst->audio_stream,
					audio_frame->data[0], audio_frame->nb_samples) == -1) {
					if (errno == EAGAIN) {
						continue;
					} else {
						LOG_VPRINT_ERROR("Could not write audio frames: %s",
							strerror(errno));
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
						break;
					}
				}
				av_frame_unref(audio_frame);
			}
		}

		/* remove packet from queue */
		if (packet != NULL) {
			if (avbox_queue_get(inst->audio_packets_q) != packet) {
				LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result (%p): %s",
					packet, strerror(errno));
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT,
					&inst->audio_decoder_worker);
				goto end;
			}
			/* free packet */
			av_packet_unref(packet);
			free(packet);
			packet = NULL;
		}

		if (just_flushed) {
			DEBUG_PRINT(LOG_MODULE, "Flushing audio codec");
			avcodec_flush_buffers(inst->audio_codec_ctx);
			inst->audio_decoder_flushed = 1;
			just_flushed = 0;
		}
	}
end:
	DEBUG_PRINT("player", "Audio decoder exiting");

	avbox_checkpoint_disable(&inst->audio_decoder_checkpoint);

	if (audio_buffersink_ctx != NULL) {
		DEBUG_PRINT("player", "Flushing audio filter graph");
		while ((ret = av_buffersink_get_frame(audio_buffersink_ctx, audio_frame)) >= 0) {
			av_frame_unref(audio_frame);
		}
		if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
			char err[256];
			av_strerror(ret, err, sizeof(err));
			LOG_VPRINT_ERROR("Could not audio flush filter graph: %s", err);
		}
		avfilter_free(audio_buffersrc_ctx);
	}
	if (audio_buffersink_ctx != NULL) {
		avfilter_free(audio_buffersink_ctx);
	}
	if (audio_filter_graph != NULL) {
		avfilter_graph_free(&audio_filter_graph);
	}
	if (audio_frame_nat != NULL) {
		av_free(audio_frame_nat);
	}
	if (audio_frame != NULL) {
		av_free(audio_frame);
	}
	if (inst->audio_codec_ctx != NULL) {
		DEBUG_PRINT("player", "Flushing audio decoder");
		avcodec_flush_buffers(inst->audio_codec_ctx);
		avcodec_close(inst->audio_codec_ctx);
		avcodec_free_context(&inst->audio_codec_ctx);
		inst->audio_codec_ctx = NULL; /* avcodec_free_context() already does this */
	}

	DEBUG_PRINT("player", "Audio decoder bailing out");

	return NULL;
}


#ifdef ENABLE_DVD
/**
 * Handles DVDNAV events
 */
static void
avbox_player_dvdnav_handler(void * const context, int event, void *data)
{
	struct avbox_player * const inst = context;
	struct avbox_player_waiter waiter;

	if (pthread_mutex_init(&waiter.mutex, NULL) != 0 ||
		pthread_cond_init(&waiter.cond, NULL) != 0) {
		abort();
	}

	/* send the DVDNAV event to the control thread and wait
	 * for it to react */
	waiter.data = data;
	pthread_mutex_lock(&waiter.mutex);
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_DVD | event, &waiter);
	pthread_cond_wait(&waiter.cond, &waiter.mutex);
	pthread_mutex_unlock(&waiter.mutex);
}
#endif


static void
avbox_player_settitle(struct avbox_player * const inst, const char * const title)
{
	char * const new_title = strdup(title);
	if (new_title == NULL) {
		LOG_PRINT_ERROR("Could not set title. Out of memory");
	} else {
		pthread_mutex_lock(&inst->state_lock);
		free(inst->state_info.title);
		inst->state_info.title = new_title;
		pthread_mutex_unlock(&inst->state_lock);
	}
}


static void
avbox_player_audiostream_dried(struct avbox_audiostream * stream, void * const context)
{
	struct avbox_player * const inst = context;
	ASSERT(inst->audio_stream == stream);
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_AUDIO_STREAM_DRIED, NULL);
}


/**
 * This is the main decoding loop. It reads the stream and feeds
 * encoded frames to the decoder threads.
 */
static void*
avbox_player_stream_parse(void *arg)
{
	int res;
	AVPacket *ppacket;
	AVDictionary *stream_opts = NULL;
	struct avbox_player *inst = (struct avbox_player*) arg;
	int prefered_video_stream = -1;
	const char *file_to_open;

	DEBUG_SET_THREAD_NAME("stream_parser");

	ASSERT(inst != NULL);
	ASSERT(inst->media_file != NULL);
	ASSERT(inst->window != NULL);
	ASSERT(inst->status == MB_PLAYER_STATUS_PLAYING || inst->status == MB_PLAYER_STATUS_BUFFERING);
	ASSERT(inst->fmt_ctx == NULL);
	ASSERT(inst->audio_stream == NULL);
	ASSERT(inst->audio_time_set == 0);
	ASSERT(inst->video_packets_q == NULL);
	ASSERT(inst->video_frames_q == NULL);
	ASSERT(inst->audio_packets_q == NULL);
	ASSERT(inst->audio_stream_index == -1);
	ASSERT(inst->video_stream_index == -1);

	file_to_open = inst->media_file;

	DEBUG_VPRINT("player", "Attempting to play '%s'", inst->media_file);

	avbox_player_settitle(inst, inst->media_file);

	/* allocate format context */
	if ((inst->fmt_ctx = avformat_alloc_context()) == NULL) {
		LOG_PRINT_ERROR("Could not allocate format context!");
		goto decoder_exit;
	}

#ifdef ENABLE_DVD
	if (!strncmp("dvd:", inst->media_file, 4)) {
		ASSERT(inst->dvdio == NULL);

		if ((inst->dvdio = avbox_dvdio_open(inst->media_file + 4, avbox_player_dvdnav_handler, inst)) == NULL) {
			LOG_VPRINT_ERROR("Could not open DVD: %s",
				strerror(errno));
			avformat_free_context(inst->fmt_ctx);
			inst->fmt_ctx = NULL;
			goto decoder_exit;
		}
		if ((inst->fmt_ctx->pb = avbox_dvdio_avio(inst->dvdio)) == NULL) {
			LOG_VPRINT_ERROR("Could not get avio: %s",
				strerror(errno));
			avformat_free_context(inst->fmt_ctx);
			avbox_dvdio_close(inst->dvdio);
			inst->fmt_ctx = NULL;
			inst->dvdio = NULL;
			goto decoder_exit;
		}
		file_to_open = inst->media_file + 4;
		inst->logical_audio_stream = -1;

		if (avbox_input_grab(inst->control_object) == -1) {
			abort();
		}
	}
#endif

	/* open file */
	av_dict_set(&stream_opts, "timeout", "30000000", 0);
	if (avformat_open_input(&inst->fmt_ctx, file_to_open, NULL, &stream_opts) != 0) {
		LOG_VPRINT_ERROR("Could not open stream '%s'",
			inst->media_file);
		goto decoder_exit;
	}

	if (avformat_find_stream_info(inst->fmt_ctx, NULL) < 0) {
		LOG_PRINT_ERROR("Could not find stream info!");
		goto decoder_exit;
	}

#ifdef ENABLE_DVD
	if (inst->dvdio == NULL)
#endif
	{
		AVDictionaryEntry *title_entry;

		/* update the stream title */
		if (inst->fmt_ctx->metadata != NULL) {
			if ((title_entry = av_dict_get(inst->fmt_ctx->metadata, "title", NULL, 0)) != NULL) {
				if (title_entry->value != NULL) {
					avbox_player_settitle(inst, title_entry->value);
				}
			}
		}

		inst->state_info.duration = inst->fmt_ctx->duration;
	}

	/* if there's an audio stream start the audio decoder */
	if ((inst->audio_stream_index =
		av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, prefered_video_stream, NULL, 0)) >= 0) {

		DEBUG_PRINT("player", "Audio stream found");

		/* allocate filtered audio frames */
		inst->getmastertime = avbox_player_getaudiotime; /* video is slave to audio */

		/* create audio stream */
		if ((inst->audio_stream = avbox_audiostream_new(
			AVBOX_BUFFER_AUDIO,
			avbox_player_audiostream_dried, inst)) == NULL) {
			goto decoder_exit;
		}

		if ((inst->audio_packets_q = avbox_queue_new(MB_AUDIO_BUFFER_PACKETS)) == NULL) {
			LOG_VPRINT_ERROR("Could not create audio packets queue: %s!",
				strerror(errno));
			goto decoder_exit;
		}
	} else {
		if (inst->audio_stream_index == AVERROR_DECODER_NOT_FOUND) {
			LOG_PRINT_ERROR("Could not find decoder for audio stream!");
		}
		inst->audio_stream_index = -1;
	}

	/* if the file contains a video stream fire the video decoder */
	if ((inst->video_stream_index =
		av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_VIDEO, prefered_video_stream, -1, NULL, 0)) >= 0) {
		if (inst->audio_stream_index == -1) {
			inst->getmastertime = avbox_player_getsystemtime;
		}

		/* create a video packets queue */
		if ((inst->video_packets_q = avbox_queue_new(MB_VIDEO_BUFFER_PACKETS)) == NULL) {
			LOG_VPRINT_ERROR("Could not create video packets queue: %s!",
				strerror(errno));
			goto decoder_exit;
		}

		/* create a decoded frames queue */
		if ((inst->video_frames_q = avbox_queue_new(AVBOX_BUFFER_VIDEO)) == NULL) {
			LOG_VPRINT_ERROR("Could not create frames queue: %s!",
				strerror(errno));
			goto decoder_exit;
		}

		/* if the audio is running the show then set the
		 * video queue to unlimited size */
		if (inst->audio_stream_index != -1) {
			avbox_queue_setsize(inst->video_frames_q, 0);
		}

		DEBUG_VPRINT("player", "Video stream %i selected",
			inst->video_stream_index);
	}

	/* if there's no streams to decode then exit */
	if (inst->audio_stream_index == -1 && inst->video_stream_index == -1) {
		LOG_PRINT_ERROR("No streams to decode!");
		goto decoder_exit;
	}

	/* enable checkpoint */
	avbox_checkpoint_enable(&inst->stream_parser_checkpoint);
	avbox_checkpoint_halt(&inst->stream_parser_checkpoint);

	/* notify control thread that we're ready */
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STREAM_READY,
		&inst->stream_thread);

	DEBUG_PRINT("player", "Stream decoder ready");

	/* start decoding */
	while (LIKELY(!inst->stream_quit)) {

		avbox_checkpoint_here(&inst->stream_parser_checkpoint);

		if (!inst->gotpacket) {
			if (UNLIKELY((res = av_read_frame(inst->fmt_ctx, &inst->packet)) < 0)) {
				char buf[256];
				av_strerror(res, buf, sizeof(buf));
				LOG_VPRINT_ERROR("Could not read frame: %s", buf);
				goto decoder_exit;
			}
			inst->gotpacket = 1;
		}

#ifdef ENABLE_DVD
		if (inst->dvdio == NULL)
#endif
		{
			inst->state_info.pos = av_rescale_q(inst->packet.pts,
				inst->fmt_ctx->streams[inst->packet.stream_index]->time_base,
				AV_TIME_BASE_Q);
		}

		if (inst->packet.stream_index == inst->video_stream_index) {
			if ((ppacket = malloc(sizeof(AVPacket))) == NULL) {
				LOG_PRINT_ERROR("Could not allocate memory for packet!");
				av_packet_unref(&inst->packet);
				inst->gotpacket = 0;
				goto decoder_exit;
			}
			memcpy(ppacket, &inst->packet, sizeof(AVPacket));
			if (avbox_queue_put(inst->video_packets_q, ppacket) == -1) {
				if (errno == EAGAIN) {
					free(ppacket);
					continue;
				} else if (errno == ESHUTDOWN) {
					LOG_PRINT_ERROR("Video packets queue shutdown! Aborting parser!");
					av_packet_unref(ppacket);
					free(ppacket);
					inst->gotpacket = 0;
					goto decoder_exit;
				}
				LOG_VPRINT_ERROR("Could not add packet to queue: %s",
					strerror(errno));
				av_packet_unref(ppacket);
				free(ppacket);
				inst->gotpacket = 0;
				goto decoder_exit;
			}

		} else if (inst->packet.stream_index == inst->audio_stream_index) {
			if ((ppacket = malloc(sizeof(AVPacket))) == NULL) {
				LOG_PRINT_ERROR("Could not allocate memory for packet!");
				av_packet_unref(&inst->packet);
				inst->gotpacket = 0;
				goto decoder_exit;
			}
			memcpy(ppacket, &inst->packet, sizeof(AVPacket));
			if (avbox_queue_put(inst->audio_packets_q, ppacket) == -1) {
				if (errno == EAGAIN) {
					free(ppacket);
					continue;
				} else if (errno == ESHUTDOWN) {
					LOG_PRINT_ERROR("Audio packets queue shutdown! Aborting parser!");
					av_packet_unref(ppacket);
					free(ppacket);
					inst->gotpacket = 0;
					goto decoder_exit;
				}
				LOG_VPRINT_ERROR("Could not enqueue packet: %s",
					strerror(errno));
				av_packet_unref(ppacket);
				free(ppacket);
				inst->gotpacket = 0;
				goto decoder_exit;
			}
		} else {
			av_packet_unref(&inst->packet);
		}
		inst->gotpacket = 0;
	}

decoder_exit:
	DEBUG_VPRINT("player", "Stream parser exiting (quit=%i)",
		inst->stream_quit);

	/* disable the checkpoint */
	avbox_checkpoint_disable(&inst->stream_parser_checkpoint);

	pthread_mutex_lock(&inst->state_lock);
	inst->stream_exiting  = 1;
	pthread_mutex_unlock(&inst->state_lock);

	if (inst->gotpacket) {
		av_packet_unref(&inst->packet);
	}

	if (inst->video_stream_index != -1) {
		ASSERT(inst->video_packets_q != NULL);
		avbox_queue_close(inst->video_packets_q);
	}
	if (inst->audio_stream_index != -1) {
		ASSERT(inst->audio_packets_q != NULL);
		avbox_queue_close(inst->audio_packets_q);
	}

	if (stream_opts != NULL) {
		av_dict_free(&stream_opts);
	}

	inst->stream_quit = 1;


	/* I don't think there's any benefit in doing this always
	 * but it helps in debugging as all freed memory is returned to
	 * the kernel so we get a better picture */
#if !defined(NDEBUG) && defined(HAVE_MALLOC_TRIM)
	malloc_trim(0);
#endif

	avbox_player_settitle(inst, "-");
	inst->state_info.duration = 0;

	pthread_mutex_lock(&inst->state_lock);
	inst->stream_exiting = 0;
	pthread_mutex_unlock(&inst->state_lock);

	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STREAM_EXIT,
		&inst->stream_thread);

	DEBUG_PRINT("player", "Stream parser thread bailing out");

	return NULL;
}


static inline int
avbox_player_stream_checkpoint_wait(struct avbox_player * const inst, int64_t timeout)
{
#ifdef ENABLE_DVD
	if (inst->dvdio == NULL) {
		return avbox_checkpoint_wait(&inst->stream_parser_checkpoint, timeout);
	} else {
		return avbox_dvdio_isblocking(inst->dvdio) ||
			avbox_checkpoint_wait(&inst->stream_parser_checkpoint, timeout);
	}
#else
	return avbox_checkpoint_wait(&inst->stream_parser_checkpoint, timeout);
#endif
}

/**
 * Complete halt all stages of the decoding pipeline.
 */
static void
avbox_player_halt(struct avbox_player * const inst)
{
	DEBUG_PRINT(LOG_MODULE, "Player halting...");
	ASSERT(inst->status != MB_PLAYER_STATUS_PAUSED);

	avbox_checkpoint_halt(&inst->stream_parser_checkpoint);
	do {
		avbox_queue_wake(inst->audio_packets_q);
		avbox_queue_wake(inst->video_packets_q);
	} while (!avbox_player_stream_checkpoint_wait(inst, 50L * 1000L));

	if (inst->audio_stream_index != -1) {
		avbox_checkpoint_halt(&inst->audio_decoder_checkpoint);
		do {
			avbox_queue_wake(inst->audio_packets_q);
		} while (!avbox_checkpoint_wait(&inst->audio_decoder_checkpoint, 50L * 1000L));
		avbox_audiostream_pause(inst->audio_stream);
	}

	if (inst->video_stream_index != -1) {
		avbox_checkpoint_halt(&inst->video_decoder_checkpoint);
		do {
			avbox_queue_wake(inst->video_packets_q);
			avbox_queue_wake(inst->video_frames_q);
		} while (!avbox_checkpoint_wait(&inst->video_decoder_checkpoint, 50L * 1000L));

		avbox_checkpoint_halt(&inst->video_output_checkpoint);
		do {
			avbox_queue_wake(inst->video_frames_q);
		} while (!avbox_checkpoint_wait(&inst->video_output_checkpoint, 50L * 1000L));
	}

	DEBUG_PRINT(LOG_MODULE, "Player halted");
}


/**
 * Resume the decoding pipeline after a call to avbox_player_halt().
 */
static void
avbox_player_continue(struct avbox_player * const inst)
{
	DEBUG_PRINT(LOG_MODULE, "Player resuming...");

	avbox_checkpoint_continue(&inst->stream_parser_checkpoint);

	if (inst->audio_stream_index != -1) {
		avbox_checkpoint_continue(&inst->audio_decoder_checkpoint);
		avbox_audiostream_resume(inst->audio_stream);
	}
	if (inst->video_stream_index != -1) {
		avbox_checkpoint_continue(&inst->video_output_checkpoint);
		avbox_checkpoint_continue(&inst->video_decoder_checkpoint);
	}

	DEBUG_PRINT(LOG_MODULE, "Player resumed");
}


static struct avbox_player_subscriber*
avbox_player_findsubscriber(const struct avbox_player * const inst,
	const struct avbox_object * const object)
{
	struct avbox_player_subscriber *subscriber;
	LIST_FOREACH(struct avbox_player_subscriber*, subscriber, &inst->subscribers) {
		if (subscriber->object == object) {
			return subscriber;
		}
	}
	return NULL;
}


/**
 * Gets a list of subscribers to player messages.
 */
static struct avbox_object **
avbox_player_subscribers(struct avbox_player * const inst)
{
	int cnt = 0;
	struct avbox_player_subscriber *subscriber;
	struct avbox_object **out, **pout;
	LIST_FOREACH(struct avbox_player_subscriber*, subscriber, &inst->subscribers) {
		cnt++;
	}
	if (cnt == 0) {
		return NULL;
	}
	if ((pout = out = malloc((cnt + 1) * sizeof(struct avbox_object*))) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}
	LIST_FOREACH(struct avbox_player_subscriber*, subscriber, &inst->subscribers) {
		*pout++ = subscriber->object;
	}
	*pout = NULL;
	return out;
}


/**
 * Send a message to all subscribers.
 */
static int
avbox_player_sendmsg(struct avbox_player * const inst,
	enum avbox_player_status status, enum avbox_player_status last_status)
{
	struct avbox_player_status_data *data;
	struct avbox_object **subscribers;
	if ((subscribers = avbox_player_subscribers(inst)) == NULL) {
		return 0;
	}
	if ((data = malloc(sizeof(struct avbox_player_status_data))) == NULL) {
		ASSERT(errno == ENOMEM);
		free(subscribers);
		return -1;
	}
	data->sender = inst;
	data->status = status;
	data->last_status = last_status;
	if (avbox_object_sendmsg(subscribers, AVBOX_MESSAGETYPE_PLAYER,
		AVBOX_DISPATCH_ANYCAST, data) == NULL) {
		LOG_VPRINT_ERROR("Could not send status notification: %s",
			strerror(errno));
		free(subscribers);
		free(data);
		return -1;
	}
	free(subscribers);
	return 0;
}


/**
 * Updates the player status and
 * calls any registered callbacks
 */
static void
avbox_player_updatestatus(struct avbox_player *inst, enum avbox_player_status status)
{
	enum avbox_player_status last_status;

	assert(inst != NULL);

	last_status = inst->status;
	inst->status = status;

	/* send status notification */
	if (avbox_player_sendmsg(inst, status, last_status) == -1) {
		LOG_VPRINT_ERROR("Could not send notification: %s",
			strerror(errno));
	}
}


/**
 * Free the internal playlist
 */
static void
avbox_player_freeplaylist(struct avbox_player* inst)
{
	struct avbox_playlist_item *item;

	assert(inst != NULL);

	inst->playlist_item = NULL;

	LIST_FOREACH_SAFE(struct avbox_playlist_item*, item, &inst->playlist, {

		LIST_REMOVE(item);

		if (item->filepath != NULL) {
			free((void*) item->filepath);
		}

		free(item);
	});
}


/**
 * Signal an exception to all subscribers.
 */
static void
avbox_player_throwexception(struct avbox_player * const inst,
	const char * const exception, ...)
{
	char buf[1024];
	va_list va;

	(void) inst;

	va_start(va, exception);
	vsnprintf(buf, sizeof(buf), exception, va);
	va_end(va);

	LOG_VPRINT_ERROR("%s", buf);
}


/**
 * Print the seek flags.
 */
static inline const char *
avbox_seekflags_tostring(int flags)
{
	switch (flags) {
	case AVBOX_PLAYER_SEEK_CHAPTER: return "AVBOX_PLAYER_SEEK_CHAPTER";
	case AVBOX_PLAYER_SEEK_ABSOLUTE: return "AVBOX_PLAYER_SEEK_ABSOLUTE";
	case AVBOX_PLAYER_SEEK_RELATIVE: return "AVBOX_PLAYER_SEEK_RELATIVE";
	default: return "";
	}
}


/**
 * Pause the running stream.
 */
static void
avbox_player_dopause(struct avbox_player * const inst)
{
	/* wait for player to pause */
	if (inst->audio_stream_index != -1) {
		avbox_audiostream_pause(inst->audio_stream);
	}
	if (inst->video_stream_index != -1) {
		avbox_checkpoint_halt(&inst->video_output_checkpoint);
		do {
			avbox_queue_wake(inst->video_frames_q);
		} while (!avbox_checkpoint_wait(&inst->video_output_checkpoint, 50L * 1000L));
		avbox_stopwatch_stop(inst->video_time);
	}
}


/**
 * Resume the running stream.
 */
static void
avbox_player_doresume(struct avbox_player * const inst)
{
	avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);
	if (inst->audio_stream_index != -1) {
		if (avbox_audiostream_ispaused(inst->audio_stream)) {
			avbox_audiostream_resume(inst->audio_stream);
		}
	}
	if (inst->video_stream_index != -1) {
		avbox_stopwatch_start(inst->video_time);
		avbox_checkpoint_continue(&inst->video_output_checkpoint);
	}
}


/**
 * Stop playing stream.
 */
static void
avbox_player_dostop(struct avbox_player * const inst)
{
#ifdef ENABLE_DVD
	if (inst->dvdio != NULL) {
		avbox_dvdio_close(inst->dvdio);
		return;
	}
#endif
	/* tell the stream thread to quit */
	inst->stream_quit = 1;

	/* if the video is paused then unpause it first. */
	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		DEBUG_PRINT("player", "Unpausing stream");
		avbox_player_doresume(inst);
	}
}


/**
 * Start playing a stream.
 */
static void
avbox_player_doplay(struct avbox_player * const inst,
	const char * const path)
{
	ASSERT(inst != NULL);

	/* if we're on the middle of a start/stop ignore this
	 * command */
	if (inst->play_state != AVBOX_PLAYER_PLAYSTATE_READY &&
		inst->play_state != AVBOX_PLAYER_PLAYSTATE_PLAYING) {
		avbox_player_throwexception(inst, "Ignoring play command. Current state not valid (%i)",
			inst->play_state);
		return;
	}

	/* if no path argument was provided but we're already
	 * playing a file and we're paused then just resume
	 * playback */
	if (path == NULL) {
		if (inst->status == MB_PLAYER_STATUS_PAUSED) {
			avbox_player_doresume(inst);
			return;
		}
		avbox_player_throwexception(inst, "Playback failed: NULL path!");
		return;
	}

	/* if we're already playing a file stop it first */
	if (inst->status != MB_PLAYER_STATUS_READY) {
		inst->next_file = strdup(path);
		if (inst->next_file == NULL) {
			LOG_PRINT_ERROR("Could not copy path: Out of memory");
		}
		avbox_player_dostop(inst);
		return;
	}

	/* initialize player object */
	const char *old_media_file = inst->media_file;
	inst->media_file = strdup(path);
	if (old_media_file != NULL) {
		free((void*) old_media_file);
	}

	/* update status */
	inst->stream_percent = 0;
	inst->play_state = AVBOX_PLAYER_PLAYSTATE_STREAM;
	avbox_player_updatestatus(inst, MB_PLAYER_STATUS_BUFFERING);

	/* start the main decoder thread */
	inst->stream_quit = 0;
	if (pthread_create(&inst->stream_thread, NULL, avbox_player_stream_parse, inst) != 0) {
		avbox_player_updatestatus(inst, MB_PLAYER_STATUS_READY);
		avbox_player_throwexception(inst, "Could not fire decoder thread");
		return;
	}
}


static void
avbox_player_drop(struct avbox_player * const inst)
{
	AVPacket *packet;
	AVFrame *frame;

	avbox_player_halt(inst);

	/* if the stream parser already has a packet out
	 * free it */
	if (inst->gotpacket) {
		av_packet_unref(&inst->packet);
		inst->gotpacket = 0;
	}

	/* drop all decoded video frames */
	if (inst->video_stream_index != -1) {
		/* drop all video packets */
		while (avbox_queue_count(inst->video_packets_q) > 0) {
			packet = avbox_queue_get(inst->video_packets_q);
			av_packet_unref(packet);
			free(packet);
		}

		/* Drop all decoded video frames */
		while (avbox_queue_count(inst->video_frames_q) > 0) {
			frame = avbox_queue_get(inst->video_frames_q);
			av_frame_unref(frame);
			av_free(frame);
		}

		/* flush video decoder */
		avcodec_flush_buffers(inst->video_codec_ctx);
	}

	/* drop all decoded audio frames */
	if (inst->audio_stream_index != -1) {
		/* drop all audio packets */
		while (avbox_queue_count(inst->audio_packets_q) > 0) {
			packet = avbox_queue_get(inst->audio_packets_q);
			av_packet_unref(packet);
			free(packet);
		}

		/* drop all decoded audio frames */
		avbox_audiostream_drop(inst->audio_stream);
		if (!avbox_delegate_finished(inst->audio_decoder_worker)) {
			avcodec_flush_buffers(inst->audio_codec_ctx);
		} else {
			ASSERT(inst->audio_decoder_flushed);
		}
		inst->audio_time_set = 0;

		DEBUG_VPRINT("player", "Audio time: %li",
			avbox_audiostream_gettime(inst->audio_stream));
	}

	DEBUG_VPRINT("player", "Frames dropped. (time=%li,v_packets=%i,a_packets=%i,v_frames=%i)",
		inst->getmastertime(inst), avbox_queue_count(inst->video_packets_q),
		avbox_queue_count(inst->audio_packets_q), avbox_queue_count(inst->video_frames_q));

	/* make sure everything is ok */
	ASSERT(avbox_queue_count(inst->video_packets_q) == 0);
	ASSERT(avbox_queue_count(inst->audio_packets_q) == 0);
	ASSERT(avbox_queue_count(inst->video_frames_q) == 0);
	ASSERT(avbox_audiostream_count(inst->audio_stream) == 0);

	DEBUG_VPRINT("player", "Seeking (newpos=%li)",
		inst->getmastertime(inst));

	/* flush stream buffers */
	avformat_flush(inst->fmt_ctx);

	avbox_player_continue(inst);
}


/**
 * Seek the current stream.
 */
static void
avbox_player_doseek(struct avbox_player * const inst,
	int flags, int64_t incr)
{
	int64_t seek_to;
	int64_t pos;


	ASSERT(inst != NULL);
	DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_SEEK");

	DEBUG_VPRINT("player", "Seeking (mode=%s | incr=%i)",
		avbox_seekflags_tostring(flags), incr);

	if (inst->status != MB_PLAYER_STATUS_PLAYING &&
		inst->status != MB_PLAYER_STATUS_PAUSED) {
		avbox_player_throwexception(inst, "Cannot seek: not playing");
		return;
	}

	ASSERT(inst->fmt_ctx != NULL);
	ASSERT(inst->getmastertime != NULL);

	pos = inst->getmastertime(inst);

	if (flags & AVBOX_PLAYER_SEEK_CHAPTER) {

		int chapter = 0, nb_chapters = inst->fmt_ctx->nb_chapters;
		const int64_t chapter_duration = 60 * 5 * 1000L * 1000L;

		/* find the current chapter */
		if (nb_chapters == 0) {
			int64_t p = 0;
			while (p < inst->fmt_ctx->duration) {
				if (p <= pos) {
					chapter = nb_chapters;
				}
				p += chapter_duration;
				nb_chapters++;
			}
			DEBUG_VPRINT(LOG_MODULE, "Current chapter: %i",
				chapter);
		} else {
			for (chapter = 0; chapter < inst->fmt_ctx->nb_chapters; chapter++) {
				AVChapter *ch = inst->fmt_ctx->chapters[chapter];
				if (av_compare_ts(pos, AV_TIME_BASE_Q,
					ch->start, ch->time_base) < 0) {
					chapter--;
					break;
				}
			}
		}

		DEBUG_VPRINT(LOG_MODULE, "Chapter %i of %i",
			chapter, nb_chapters);

		/* if we're seeking past the current playlist iten find the
		 * next/prev item and play it */
		if (inst->playlist_item != NULL) {
			if (incr > 0 && (nb_chapters == 0 ||
				chapter == (nb_chapters - 1))) {
				/* seek to next playlist item */
				struct avbox_playlist_item *next_item =
					inst->playlist_item;

				while (incr--) {
					struct avbox_playlist_item *next =
						LIST_NEXT(struct avbox_playlist_item*,
						inst->playlist_item);
					if (LIST_ISNULL(&inst->playlist, next)) {
						break;
					}
					next_item = next;
				}

				if (next_item != inst->playlist_item) {
					inst->playlist_item = next_item;
					avbox_player_play(inst, inst->playlist_item->filepath);
					return;
				} else {
					avbox_player_throwexception(inst, "Cannot seek: end of playlist");
					return;
				}
			} else if (incr < 0 && chapter == 0) {
				/* seek to previous playlist item */
				struct avbox_playlist_item *next_item =
					inst->playlist_item;

				while (incr++) {
					struct avbox_playlist_item *next =
						LIST_PREV(struct avbox_playlist_item*,
						inst->playlist_item);
					if (LIST_ISNULL(&inst->playlist, next)) {
						break;
					}
					next_item = next;
				}

				if (next_item != inst->playlist_item) {
					inst->playlist_item = next_item;
					avbox_player_play(inst,
						inst->playlist_item->filepath);
					return;
				} else {
					avbox_player_throwexception(inst, "Cannot seek: start of playlist");
					return;
				}
			}
		}

		chapter += incr;
		if (chapter < 0 || chapter > nb_chapters) {
			avbox_player_throwexception(inst, "Cannot seek: bad math");
			return;
		}

		if (nb_chapters == inst->fmt_ctx->nb_chapters) {
			seek_to = av_rescale_q(
				inst->fmt_ctx->chapters[chapter]->start,
				inst->fmt_ctx->chapters[chapter]->time_base,
				AV_TIME_BASE_Q);
		} else {
			seek_to = chapter * chapter_duration;
		}
	} else if (flags & AVBOX_PLAYER_SEEK_ABSOLUTE) {
		seek_to = incr;
	} else if (flags & AVBOX_PLAYER_SEEK_RELATIVE) {
		seek_to = pos + incr;
	} else {
		avbox_player_throwexception(inst, "Connot seek: Invalid argument");
		return;
	}

	DEBUG_VPRINT("player", "Seeking (pos=%li, seek_to=%li, offset=%li)",
		pos, seek_to, (seek_to - pos));

	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		avbox_player_doresume(inst);
	}

	/* seek the stream */
	if (seek_to != -1) {

		int flags = 0, err;
		const int64_t seek_from = inst->getmastertime(inst);

		if (seek_to < seek_from) {
			flags |= AVSEEK_FLAG_BACKWARD;
		}

		DEBUG_VPRINT("player", "Seeking %s from %li to %li...",
			(flags & AVSEEK_FLAG_BACKWARD) ? "BACKWARD" : "FORWARD",
			seek_from, seek_to);

		avbox_player_halt(inst);

		/* do the seeking */
		if ((err = av_seek_frame(inst->fmt_ctx, -1, seek_to, flags)) < 0) {
			char buf[256];
			buf[0] = '\0';
			av_strerror(err, buf, sizeof(buf));
			avbox_player_throwexception(inst,
				"Error seeking stream: %s", buf);
		} else {

			/* drop pipeline */
			avbox_player_drop(inst);

			/* reset video time */
			if (inst->video_stream_index != -1) {
				avbox_stopwatch_reset(inst->video_time, seek_to);
				avbox_stopwatch_start(inst->video_time);
			}

			/* reset audio time */
			if (inst->audio_stream_index != -1) {
				avbox_audiostream_pause(inst->audio_stream);
				avbox_audiostream_setclock(inst->audio_stream, seek_to);
				avbox_audiostream_resume(inst->audio_stream);
				inst->audio_time_set = 0;
				DEBUG_VPRINT("player", "Audio time: %li",
					avbox_audiostream_gettime(inst->audio_stream));
			}

			/* make sure everything is ok */
			ASSERT(avbox_queue_count(inst->video_packets_q) == 0);
			ASSERT(avbox_queue_count(inst->audio_packets_q) == 0);
			ASSERT(avbox_queue_count(inst->video_frames_q) == 0);
			ASSERT(avbox_audiostream_count(inst->audio_stream) == 0);
			ASSERT(inst->getmastertime(inst) == seek_to);

			DEBUG_VPRINT("player", "Seeking (newpos=%li)",
				inst->getmastertime(inst));

			DEBUG_PRINT("player", "Seek complete");
		}

		/* resume playback */
		avbox_player_continue(inst);
	}

}


/**
 * Check if the stream has underrun.
 */
static int
avbox_player_isunderrun(struct avbox_player * const inst)
{
	int underrun = 0;
	if (!inst->stream_quit && !inst->stream_exiting) {
		if (inst->video_stream_index != -1) {
			if (inst->audio_stream_index == -1) {
				if (avbox_queue_count(inst->video_frames_q) < 1) {
					underrun = 1;
				}
			} else {
				if (avbox_audiostream_count(inst->audio_stream) < 1) {
					underrun = 1;
				}
			}
		}
		if (inst->audio_stream_index != -1) {
			/* check for audio underrun */
		}
	}
	return underrun;
}


/**
 * Handle the stream underrun.
 */
static void
avbox_player_handle_underrun(struct avbox_player * const inst)
{
	struct timespec tv;

	/* update buffer state */
	if (inst->video_stream_index != -1) {
		int avail = avbox_queue_count(inst->video_frames_q);
		const int wanted = AVBOX_BUFFER_VIDEO;
		inst->stream_percent = (((avail * 100) / wanted) * 100) / 100;
	}

	/* send status update */
	avbox_player_updatestatus(inst, MB_PLAYER_STATUS_BUFFERING);

	/* set the timer */
	tv.tv_sec = 0;
	tv.tv_nsec = 500L * 1000L;
	inst->underrun_timer_id = avbox_timer_register(&tv,
		AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
		inst->control_object, NULL, inst);
	if (inst->underrun_timer_id == -1) {
		avbox_player_throwexception(inst,
			"Could not start buffering timer");
		avbox_player_doresume(inst);
		avbox_player_updatestatus(inst,
			MB_PLAYER_STATUS_PLAYING);
	}
}


static void
avbox_player_delay_stream_exit(struct avbox_player * const inst)
{
	struct timespec tv;

	ASSERT(inst->stream_exit_timer_id == -1);

	/* set the timer */
	tv.tv_sec = 0;
	tv.tv_nsec = 500L * 1000L;
	inst->stream_exit_timer_id = avbox_timer_register(&tv,
		AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
		inst->control_object, NULL, inst);
	if (inst->stream_exit_timer_id == -1) {
		LOG_PRINT_ERROR("Could not start stream exit timer. Blocking thread!!");
		usleep(500L * 1000L);
		avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STREAM_EXIT, NULL);
	}
}


#ifdef ENABLE_DVD
static int
avbox_player_flush(struct avbox_player * const inst)
{
	if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_PLAYING) {
		if (!inst->flushing) {
			DEBUG_VPRINT(LOG_MODULE, "Flushing pipeline (pts=%li)",
				inst->getmastertime(inst));

			avbox_checkpoint_halt(&inst->stream_parser_checkpoint);
			do {
				/* if everything goes whell we don't need to
				 * do anything here ASSUMING THIS IS USED ONLY
				 * FOR DVDs!*/
			} while (!avbox_player_stream_checkpoint_wait(inst, 50L * 1000L));

			inst->flushing = 1;
		}

		/* if there's any packets on the audio pipeline
		 * return 0 */
		if (inst->audio_stream_index != -1) {
			if (!inst->audio_decoder_flushed) {
				return 0;
			}
			if (avbox_audiostream_count(inst->audio_stream) > 0 ||
				avbox_queue_count(inst->audio_packets_q) > 0) {
				return 0;
			}
		}

		/* if there's any packets or frames on the video pipeline
		 * then rturn zero. */
		if (inst->video_stream_index != -1) {
			if (inst->logical_audio_stream == -1) {
				if (avbox_queue_count(inst->video_packets_q) != 0 ||
					avbox_queue_count(inst->video_frames_q) != 0) {
					return 0;
				}
				if (!inst->video_decoder_flushed) {
					return 0;
				}

				/* make sure that the audio ring buffer is also flushed */
				if (inst->audio_stream_index != -1) {
					avbox_audiostream_pause(inst->audio_stream);
					avbox_audiostream_resume(inst->audio_stream);
				}

			} else {
				if (avbox_queue_count(inst->video_packets_q) > 0 ||
					avbox_queue_count(inst->video_frames_q) > 0 ||
					!inst->video_decoder_flushed) {
#ifndef NDEBUG
					AVFrame *last_drop = NULL;
#endif
					ASSERT(inst->audio_stream_index != -1);

					/* make sure that the audio ring buffer is also flushed */
					avbox_audiostream_pause(inst->audio_stream);
					avbox_audiostream_resume(inst->audio_stream);

					DEBUG_VPRINT(LOG_MODULE, "Audio stream dried out at pts %li",
						inst->getmastertime(inst));


					/* So we're stuck with some video frames. This happens if
					 * we try to flush at a point where we have read a video packet
					 * but not the correspoinding audio packet(s) so the frame(s) will
					 * never be shown. Therefore we force the video output thread
					 * to present them by modifient their pts. */
					while (avbox_queue_count(inst->video_frames_q) > 0 ||
						avbox_queue_count(inst->video_packets_q) > 0 ||
						!inst->video_decoder_flushed) {

						AVFrame *frame;
						avbox_checkpoint_halt(&inst->video_output_checkpoint);
						do {
							avbox_queue_wake(inst->video_frames_q);
						} while (!avbox_checkpoint_wait(&inst->video_output_checkpoint, 50L * 1000L));

						if (avbox_queue_count(inst->video_frames_q) > 0) {
							if ((frame = avbox_queue_peek(inst->video_frames_q, 0)) == NULL) {
								LOG_PRINT_ERROR("There appears to be data corruption. Aborting");
								abort();
							}
#ifndef NDEBUG
							if (last_drop != frame) {
								DEBUG_VPRINT(LOG_MODULE, "Dumping frame with timestamp: %i",
									av_rescale_q(av_frame_get_best_effort_timestamp(frame),
										inst->fmt_ctx->streams[inst->video_stream_index]->time_base,
										AV_TIME_BASE_Q));
								last_drop = frame;
							}
#endif
							frame->pts = AV_NOPTS_VALUE;
						}

						avbox_checkpoint_continue(&inst->video_output_checkpoint);
						sched_yield(); /* let output thread run */
					}
				}
			}
		}
		
		if (inst->gotpacket) {
			av_packet_unref(&inst->packet);
			inst->gotpacket = 0;
		}

		/*avio_flush(inst->fmt_ctx->pb);*/
		avformat_flush(inst->fmt_ctx);

		DEBUG_VPRINT(LOG_MODULE, "Pipeline flushed (video_packets=%i|video_frames=%i)",
			avbox_queue_count(inst->video_packets_q),
			avbox_queue_count(inst->video_frames_q));

		inst->flushing = 0;
		avbox_checkpoint_continue(&inst->stream_parser_checkpoint);
		return 1;

	} else if (inst->play_state == AVBOX_PLAYER_PLAYSTATE_STREAM) {
		/* nothing to flush yet */
		return 1;
	} else {
		DEBUG_VPRINT(LOG_MODULE, "Flushing on state %i wtf?",
			inst->play_state);
		abort();
	}
}


static void
avbox_player_process_menus(struct avbox_player * const inst)
{
	dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
	int32_t btnid;
	pci_t *pci;

	pci = dvdnav_get_current_nav_pci(dvdnav);
	dvdnav_get_current_nav_dsi(dvdnav);
	dvdnav_get_current_highlight(dvdnav, &btnid);

	if (pci->hli.hl_gi.btn_ns > (btnid - 1)) {
		btni_t *btn = &(pci->hli.btnit[btnid - 1]);
		inst->highlight.x = btn->x_start;
		inst->highlight.y = btn->y_end;
		inst->highlight.w = btn->x_end - btn->x_start;
		inst->highlight.h = 5;
		avbox_window_update(inst->window);
	} else {
		if (inst->highlight.x != 0 || inst->highlight.y != 0) {
			inst->highlight.x = inst->highlight.y = 0;
			avbox_window_update(inst->window);
		}
	}
}
#endif


/**
 * Handles player control messages.
 */
static int
avbox_player_control(void * context, struct avbox_message * msg)
{
	const int msgid = avbox_message_id(msg);
	struct avbox_player * const inst = (struct avbox_player*) context;

	switch (msgid) {
	case AVBOX_MESSAGETYPE_USER:
	{
		struct avbox_player_ctlmsg * const ctlmsg =
			(struct avbox_player_ctlmsg*) avbox_message_payload(msg);

		switch (ctlmsg->id) {
		case AVBOX_PLAYERCTL_PLAY:
		{
			char * path = (char *) ctlmsg->data;
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_PLAY");
			avbox_player_doplay(inst, path);
			free(path);
			break;
		}
		case AVBOX_PLAYERCTL_STREAM_READY:
		{
			ASSERT(inst != NULL);
			ASSERT(inst->play_state == AVBOX_PLAYER_PLAYSTATE_STREAM);
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_STREAM_READY");

			inst->play_state = AVBOX_PLAYER_PLAYSTATE_AUDIODEC;

			/* if there's no audio just proceed to the next stage */
			if (inst->audio_stream_index == -1) {
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_AUDIODEC_READY, NULL);
			} else {
				/* start the audio decoder thread and wait until it's ready to decode */
				struct avbox_audio_decoder_args args;
				if (pthread_mutex_init(&args.mutex, NULL) != 0 ||
					pthread_cond_init(&args.cond, NULL) != 0) {
					abort();
				}
				args.inst = inst;
				pthread_mutex_lock(&args.mutex);
				if ((inst->audio_decoder_worker = avbox_thread_delegate(
					avbox_player_audio_decode, &args)) == NULL) {
					abort();
				}
				pthread_cond_wait(&args.cond, &args.mutex);
				pthread_mutex_unlock(&args.mutex);
			}
			break;
		}
		case AVBOX_PLAYERCTL_AUDIODEC_READY:
		{
			ASSERT(inst != NULL);
			ASSERT(inst->play_state == AVBOX_PLAYER_PLAYSTATE_AUDIODEC);
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_AUDIODEC_READY");

			inst->play_state = AVBOX_PLAYER_PLAYSTATE_VIDEODEC;

			if (inst->video_stream_index == -1) {
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_VIDEODEC_READY, NULL);
			} else {
				if (pthread_create(&inst->video_decoder_thread, NULL, avbox_player_video_decode, inst) != 0) {
					LOG_PRINT_ERROR("Could not create video decoder thread!");
					abort();
				}
			}
			break;
		}
		case AVBOX_PLAYERCTL_VIDEODEC_READY:
		{
			ASSERT(inst != NULL);
			ASSERT(inst->play_state == AVBOX_PLAYER_PLAYSTATE_VIDEODEC);
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_VIDEODEC_READY");

			inst->play_state = AVBOX_PLAYER_PLAYSTATE_AUDIOOUT;

			if (inst->audio_stream_index == -1) {
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_AUDIOOUT_READY, NULL);
			} else {
				if (avbox_audiostream_start(inst->audio_stream) == -1) {
					ASSERT(errno != EEXIST);
					LOG_PRINT_ERROR("Could not start audio stream");
				}
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_AUDIOOUT_READY, NULL);
			}
			break;
		}
		case AVBOX_PLAYERCTL_AUDIOOUT_READY:
		{
			ASSERT(inst != NULL);
			ASSERT(inst->play_state == AVBOX_PLAYER_PLAYSTATE_AUDIOOUT);
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_AUDIOOUT_READY");

			inst->play_state = AVBOX_PLAYER_PLAYSTATE_VIDEOOUT;

			avbox_stopwatch_reset(inst->video_time, 0);
			avbox_stopwatch_start(inst->video_time);

			if (inst->video_stream_index == -1) {
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_VIDEOOUT_READY, NULL);
			} else {
				if (pthread_create(&inst->video_output_thread, NULL, avbox_player_video, inst) != 0) {
					abort();
				}
			}

			break;
		}
		case AVBOX_PLAYERCTL_VIDEOOUT_READY:
		{
			ASSERT(inst != NULL);
			ASSERT(inst->play_state == AVBOX_PLAYER_PLAYSTATE_VIDEOOUT);
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_VIDEOOUT_READY");
			inst->play_state = AVBOX_PLAYER_PLAYSTATE_PLAYING;
#ifdef ENABLE_DVD
			if (inst->dvdio != NULL) {
				const char *title;
				avformat_flush(inst->fmt_ctx);
				avbox_dvdio_play(inst->dvdio);
				if (dvdnav_get_title_string(
					avbox_dvdio_dvdnav(inst->dvdio), &title) == DVDNAV_STATUS_OK) {
					avbox_player_settitle(inst, title);
				}
			}
#endif
			avbox_checkpoint_continue(&inst->stream_parser_checkpoint);
			avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);
			break;
		}
		case AVBOX_PLAYERCTL_STREAM_EXIT:
		{
			ASSERT(inst != NULL);
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_STREAM_EXIT");

			/* if we're buffering then resume */
			if (inst->underrun_timer_id != -1) {
				avbox_timer_cancel(inst->underrun_timer_id);
				inst->underrun_timer_id = -1;
				avbox_player_doresume(inst);
			}

			/* if this is a user requested stop (as opposed to a
			 * stream reaching EOF) lets drop the pipelines to stop
			 * fast */
			if (inst->play_state == AVBOX_PLAYER_PLAYSTATE_STOPPING) {
				avbox_player_drop(inst);
			}

			if (inst->audio_stream_index != -1) {
				ASSERT(inst->audio_stream != NULL);
				if (avbox_audiostream_count(inst->audio_stream) > 0 ||
					avbox_queue_count(inst->audio_packets_q) > 0) {
					avbox_player_delay_stream_exit(inst);
					free(ctlmsg);
					return AVBOX_DISPATCH_OK;
				}
				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_AUDIOOUT) {
					avbox_stopwatch_reset(inst->video_time,
						avbox_audiostream_gettime(inst->audio_stream));
					avbox_stopwatch_start(inst->video_time);
					inst->getmastertime = avbox_player_getsystemtime;
				}
				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_AUDIODEC) {
					avbox_delegate_wait(inst->audio_decoder_worker, NULL);
				}
				avbox_queue_destroy(inst->audio_packets_q);
				inst->audio_packets_q = NULL;
				inst->audio_stream_index = -1;
				inst->audio_time_set = 0;
			}

			if (inst->video_stream_index != -1) {
				if (avbox_queue_count(inst->video_frames_q) > 0 ||
					avbox_queue_count(inst->video_packets_q) > 0) {
					avbox_player_delay_stream_exit(inst);
					free(ctlmsg);
					return AVBOX_DISPATCH_OK;
				}

				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_VIDEOOUT) {
					pthread_join(inst->video_output_thread, NULL);
				}
				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_VIDEODEC) {
					pthread_join(inst->video_decoder_thread, NULL);
				}

				/* TODO: This should be done by the video decoder thread,
				 * however it is currently used by the output thread */
				if (inst->video_codec_ctx != NULL) {
					avcodec_close(inst->video_codec_ctx);
					avcodec_free_context(&inst->video_codec_ctx);
					inst->video_codec_ctx = NULL; /* avcodec_free_context() already does this */
				}

				if (inst->video_window != NULL) {
					avbox_window_destroy(inst->video_window);
					inst->video_window = NULL;
				}

				avbox_queue_destroy(inst->video_frames_q);
				avbox_queue_destroy(inst->video_packets_q);
				inst->video_frames_q = NULL;
				inst->video_packets_q = NULL;
				inst->video_stream_index = -1;
			}

			if (inst->audio_stream != NULL) {
				avbox_audiostream_destroy(inst->audio_stream);
				inst->audio_stream = NULL;
			}

			/* join the stream thread */
			pthread_join(inst->stream_thread, NULL);

			/* clean other stuff */
			if (inst->fmt_ctx != NULL) {
				avformat_close_input(&inst->fmt_ctx);
				/* avformat_free_context(inst->fmt_ctx); */ /* not sure if we need this */
				inst->fmt_ctx = NULL;
			}

#ifdef ENABLE_DVD
			if (inst->dvdio != NULL) {
				avbox_input_release(inst->control_object);
				inst->highlight.x = inst->highlight.y = 0;
				avbox_dvdio_close(inst->dvdio);
				avbox_dvdio_destroy(inst->dvdio);
				inst->dvdio = NULL;
			}
#endif

			avbox_player_updatestatus(inst, MB_PLAYER_STATUS_READY);

			/* if this is a playlist and the STOP wasn't requested
			 * then play the next item */
			if (inst->play_state != AVBOX_PLAYER_PLAYSTATE_STOPPING) {
				if (inst->next_file != NULL) {
					avbox_player_play(inst, inst->next_file);
					inst->next_file = NULL; /* freed by play */

				} else if (inst->playlist_item != NULL) {
					inst->playlist_item = LIST_NEXT(struct avbox_playlist_item*,
						inst->playlist_item);
					if (!LIST_ISNULL(&inst->playlist, inst->playlist_item)) {
						avbox_player_play(inst, inst->playlist_item->filepath);
					}
				}
			}

			inst->play_state = AVBOX_PLAYER_PLAYSTATE_READY;

			break;
		}
		case AVBOX_PLAYERCTL_PAUSE:
		{
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_PAUSE");
			ASSERT(inst != NULL);

			/* can't pause if we're not playing */
			if (inst->status != MB_PLAYER_STATUS_PLAYING) {
				avbox_player_throwexception(inst, "Cannot pause: Not playing!");
				break;
			}

#ifdef ENABLE_DVD
			/* if this is a DVD then we can only pause while playing a
			 * VTS. Since mediabox sends the pause command when the play
			 * button is pressed (ie. it has a combined play/pause button
			 * we assume that the user pressed PLAY on a menu and activate
			 * the selected entry. TODO: Perhaps this can be controlled by
			 * a flag to allow for applications with a separate play and
			 * pause button */
			if (inst->dvdio) {
				dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
				ASSERT(dvdnav != NULL);
				if (!dvdnav_is_domain_fp(dvdnav) && !dvdnav_is_domain_vts(dvdnav)) {
					dvdnav_button_activate(dvdnav, dvdnav_get_current_nav_pci(dvdnav));
					break;
				}
			}
#endif
			/* update status and pause */
			avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PAUSED);
			avbox_player_dopause(inst);
			break;
		}
		case AVBOX_PLAYERCTL_STOP:
		{
			ASSERT(inst != NULL);
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_STOP");
			if (inst->play_state == AVBOX_PLAYER_PLAYSTATE_READY) {
				avbox_player_throwexception(inst, "Cannot stop: Nothing to stop!");
				break;
			}
			inst->play_state = AVBOX_PLAYER_PLAYSTATE_STOPPING;
			avbox_player_dostop(inst);
			break;
		}
		case AVBOX_PLAYERCTL_SEEK:
		{
			struct avbox_player_seekargs * const args =
				(struct avbox_player_seekargs*) ctlmsg->data;
#ifdef ENABLE_DVD
			if (inst->dvdio != NULL) {
				int32_t current_title, current_part, next_part, n_parts;
				dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
				ASSERT(dvdnav != NULL);

				if (dvdnav_current_title_info(dvdnav, &current_title, &current_part) != DVDNAV_STATUS_OK) {
					LOG_VPRINT_ERROR("Could not get DVD title info: %s",
						dvdnav_err_to_string(dvdnav));
					break;
				}

				if (current_title == -1) {
					LOG_PRINT_ERROR("Cannot seek. Currently in a menu?");
					break;
				}

				if (dvdnav_get_number_of_parts(dvdnav, current_title, &n_parts) != DVDNAV_STATUS_OK) {
					LOG_VPRINT_ERROR("Could not get number of parts in DVD title: %s",
						dvdnav_err_to_string(dvdnav));
					break;
				}

				next_part = current_part + args->pos;

				if (next_part > (n_parts - 1)) {
					LOG_PRINT_ERROR("Cannot seek. Already at last part");
					break;
				} else if (next_part < 0) {
					LOG_PRINT_ERROR("Cannot seek before start.");
					break;
				}

				if (dvdnav_part_play(dvdnav, current_title, next_part) != DVDNAV_STATUS_OK) {
					LOG_VPRINT_ERROR("Could not seek to part %i: %s",
						next_part, dvdnav_err_to_string(dvdnav));
					break;
				}

				break;
			}
#endif
			avbox_player_doseek(inst, args->flags, args->pos);
			free(args);
			break;
		}
		case AVBOX_PLAYERCTL_BUFFER_UNDERRUN:
		{
#ifdef ENABLE_DVD
			/* underrun cases problem with DVD playback and
			 * they shouldn't underrun anyways (when it happens it's
			 * because we're flushing TODO: maybe we can check if we're
			 * flushing instead, that way we can use avbox_player_flush()
			 * outside DVDs */
			if (inst->dvdio != NULL) {
				break;
			}
#endif
			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_BUFFER_UNDERRUN");

			/* underruns are expected while stopping.
			 * no need to react */
			if (inst->play_state == AVBOX_PLAYER_PLAYSTATE_STOPPING) {
				break;
			}

			DEBUG_VPRINT(LOG_MODULE, "Current play_state: %x",
				inst->play_state);

			if (avbox_player_isunderrun(inst) &&
				inst->status != MB_PLAYER_STATUS_BUFFERING) {
				DEBUG_PRINT(LOG_MODULE, "Underrun detected!");
				avbox_player_dopause(inst);
				avbox_player_handle_underrun(inst);
			}
			break;
		}
		case AVBOX_PLAYERCTL_AUDIO_STREAM_DRIED:
		{
			if (!inst->stream_exiting && inst->play_state == AVBOX_PLAYER_PLAYSTATE_PLAYING) {
				DEBUG_PRINT(LOG_MODULE, "Audio stream dried!! Switching to video clock.");
				avbox_stopwatch_reset(inst->video_time,
					avbox_audiostream_gettime(inst->audio_stream));
				inst->getmastertime = avbox_player_getsystemtime;
				avbox_stopwatch_start(inst->video_time);
				inst->audio_time_set = 0;
			}
			break;
		}
		case AVBOX_PLAYERCTL_THREADEXIT:
		{
			LOG_PRINT_ERROR("Thread exitted unexpectedly!");
			inst->play_state = AVBOX_PLAYER_PLAYSTATE_STOPPING;
			avbox_player_dostop(inst);
			break;
		}
#ifdef  ENABLE_DVD
		case AVBOX_PLAYERCTL_DVD_HOP_CHANNEL:
		case AVBOX_PLAYERCTL_DVD_WAIT:
		{
			struct avbox_player_waiter * const waiter = ctlmsg->data;

			if (!inst->flushing) {
				DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_DVD_WAIT");
			}

			ASSERT(inst != NULL);
			ASSERT(inst->dvdio != NULL);
			ASSERT(waiter != NULL);

			/* if the player is ready flush it and tell
			 * DVDNAV to continue */
			if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_PLAYING) {
				/* we cannot flush while on underrun because
				 * the audio stream is paused so break and wait
				 * for DVDNAV to send the message again */
				if (inst->underrun_timer_id != -1) {
					goto wait_end;
				}
				while (!avbox_player_flush(inst)) {
					avbox_player_process_menus(inst);
					usleep(50L * 1000L);
				}
				dvdnav_wait_skip(avbox_dvdio_dvdnav(inst->dvdio));
			} else if (inst->play_state == AVBOX_PLAYER_PLAYSTATE_STREAM) {
				/* this is the first one so we let it go by */
				dvdnav_wait_skip(avbox_dvdio_dvdnav(inst->dvdio));
			} else {
				DEBUG_VPRINT(LOG_MODULE, "Player state: %i Aborting.",
					inst->play_state);
				abort();
			}
wait_end:
			/* signal the DVDNAV state machine to continue */
			pthread_mutex_lock(&waiter->mutex);
			pthread_cond_signal(&waiter->cond);
			pthread_mutex_unlock(&waiter->mutex);
			break;
		}
		case AVBOX_PLAYERCTL_DVD_STILL_FRAME:
		{
			struct avbox_player_waiter * const waiter = ctlmsg->data;

			ASSERT(inst != NULL);
			ASSERT(inst->dvdio != NULL);
			ASSERT(waiter != NULL);

			dvdnav_still_event_t *ev = (dvdnav_still_event_t*) waiter->data;

			if (ev->length != 0xFF) {
				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_PLAYING) {
					DEBUG_VPRINT(LOG_MODULE, "Waiting %i seconds",
						ev->length);
					while(!avbox_player_flush(inst)) {
						usleep(50L * 1000L);
					}
					sleep(ev->length);
				}
				dvdnav_still_skip(avbox_dvdio_dvdnav(inst->dvdio));

			} else {
				while(!avbox_player_flush(inst)) {
					usleep(50L * 1000L);
				}
				avbox_player_process_menus(inst);
			}

			/* signal the DVDNAV state machine to continue */
			pthread_mutex_lock(&waiter->mutex);
			pthread_cond_signal(&waiter->cond);
			pthread_mutex_unlock(&waiter->mutex);
			break;
		}
		case AVBOX_PLAYERCTL_DVD_AUDIO_STREAM_CHANGE:
		{
			struct avbox_player_waiter * const waiter = ctlmsg->data;
			dvdnav_audio_stream_change_event_t * const e = waiter->data;

			DEBUG_VPRINT(LOG_MODULE, "AVBOX_PLAYERCTL_DVD_AUDIO_STREAM_CHANGE (phys=%i|log=%i)",
				e->physical, e->logical);

			ASSERT(inst != NULL);
			ASSERT(inst->dvdio != NULL);
			ASSERT(waiter != NULL);

			const int8_t active_stream =
				dvdnav_get_active_audio_stream(avbox_dvdio_dvdnav(inst->dvdio));

			DEBUG_VPRINT(LOG_MODULE, "Active audio stream: %d",
				active_stream);

			if (inst->play_state == AVBOX_PLAYER_PLAYSTATE_PLAYING) {

				/* flush the pipeline */
				while (!avbox_player_flush(inst)) {
					usleep(50L * 1000L);
				}

				inst->logical_audio_stream = active_stream;

				if (inst->audio_stream_index != -1) {

					if (active_stream != -1) {
						/* drain the audio stream and reset it's clock */
						avbox_audiostream_pause(inst->audio_stream);
						avbox_audiostream_setclock(inst->audio_stream, 0);
						avbox_audiostream_resume(inst->audio_stream);
						inst->audio_time_set = 0;

						/* audio is master so make video buffer unbound */
						if (inst->video_stream_index != -1) {
							avbox_queue_setsize(inst->video_frames_q, 0);
						}
					} else {
						/* reset the video clock */
						avbox_stopwatch_reset(inst->video_time, 0);
						avbox_stopwatch_start(inst->video_time);
						inst->getmastertime = avbox_player_getsystemtime;

						/* set video buffer limits */
						if (inst->video_stream_index != -1) {
							avbox_queue_setsize(inst->video_frames_q, AVBOX_BUFFER_VIDEO);
						}
					}

					/* stop the audio decode thread */
					avbox_queue_close(inst->audio_packets_q);
					do {
						avbox_queue_wake(inst->audio_packets_q);
						usleep(50L * 1000L);
					} while (!avbox_delegate_finished(inst->audio_decoder_worker));
					avbox_delegate_wait(inst->audio_decoder_worker, NULL);
					avbox_queue_destroy(inst->audio_packets_q);
					if ((inst->audio_packets_q = avbox_queue_new(MB_AUDIO_BUFFER_PACKETS)) == NULL) {
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
					} else {
						/* start the audio decoder thread and wait until it's ready to decode */
						struct avbox_audio_decoder_args args;
						if (pthread_mutex_init(&args.mutex, NULL) != 0 ||
							pthread_cond_init(&args.cond, NULL) != 0) {
							abort();
						}
						args.inst = inst;
						pthread_mutex_lock(&args.mutex);
						if ((inst->audio_decoder_worker = avbox_thread_delegate(
							avbox_player_audio_decode, &args)) == NULL) {
							abort();
						}
						pthread_cond_wait(&args.cond, &args.mutex);
						pthread_mutex_unlock(&args.mutex);
					}
				}
			}

			/* signal the DVDNAV state machine to continue */
			pthread_mutex_lock(&waiter->mutex);
			pthread_cond_signal(&waiter->cond);
			pthread_mutex_unlock(&waiter->mutex);
			break;
		}
		case AVBOX_PLAYERCTL_DVD_VTS_CHANGE:
		{
			struct avbox_player_waiter * const waiter = ctlmsg->data;
			int32_t current_title, current_part;
			uint32_t width, height;
			uint64_t *part_times = NULL, duration = 0;
			const char *title = NULL;
			dvdnav_t *dvdnav;
			dvdnav_vts_change_event_t * const e = waiter->data;

			DEBUG_VPRINT(LOG_MODULE, "AVBOX_PLAYERCTL_DVD_VTS_CHANGE (old=%i|new=%i)",
				e->old_vtsN, e->new_vtsN);

			(void) e;

			ASSERT(inst != NULL);
			ASSERT(inst->dvdio != NULL);
			ASSERT(waiter != NULL);

			dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
			ASSERT(dvdnav != NULL);

			if (dvdnav_get_title_string(dvdnav, &title) != DVDNAV_STATUS_OK) {
				LOG_PRINT_ERROR("Could not get dvd title!");
				goto vts_change_end;
			}

			if (dvdnav_get_video_resolution(dvdnav, &width, &height) != 0) {
				LOG_PRINT_ERROR("Could not get VTS resolution!");
				goto vts_change_end;
			}

			DEBUG_VPRINT(LOG_MODULE, "VTS Title: %s", title);
			DEBUG_VPRINT(LOG_MODULE, "VTS Resolution: %ix%i", width, height);

			if (dvdnav_current_title_info(dvdnav, &current_title, &current_part) != DVDNAV_STATUS_OK) {
				LOG_VPRINT_ERROR("Could not get DVD title info: %s",
					dvdnav_err_to_string(dvdnav));
				goto vts_change_end;
			}

			(void) dvdnav_describe_title_chapters(dvdnav, current_title,
				&part_times, &duration);
			if (part_times != NULL) {
				free(part_times);
			}

			inst->state_info.duration =
				(duration / (90L * 1000L)) * 1000L * 1000L;

			/* update player state */
			avbox_player_settitle(inst, title);

vts_change_end:
			/* signal the DVDNAV state machine to continue */
			pthread_mutex_lock(&waiter->mutex);
			pthread_cond_signal(&waiter->cond);
			pthread_mutex_unlock(&waiter->mutex);
			break;
		}
		case AVBOX_PLAYERCTL_DVD_CELL_CHANGE:
		{
			int32_t tt, ptt;
			uint32_t pos, len;
			struct avbox_player_waiter * const waiter = ctlmsg->data;
			dvdnav_t *dvdnav = avbox_dvdio_dvdnav(inst->dvdio);

			DEBUG_PRINT(LOG_MODULE, "DVDNAV_CELL_CHANGE");

			dvdnav_current_title_info(dvdnav, &tt, &ptt);
			dvdnav_get_position(dvdnav, &pos, &len);

			DEBUG_VPRINT(LOG_MODULE, "Cell change: Title %d, Chapter %d", tt, ptt);
			DEBUG_VPRINT(LOG_MODULE, "At pos %d/%d", pos, len);

			/* signal the DVDNAV state machine to continue */
			pthread_mutex_lock(&waiter->mutex);
			pthread_cond_signal(&waiter->cond);
			pthread_mutex_unlock(&waiter->mutex);
			break;
		}
		case AVBOX_PLAYERCTL_DVD_SPU_CLUT_CHANGE:
		{
			struct avbox_player_waiter * const waiter = ctlmsg->data;

			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_DVD_SPU_CLUT_CHANGE");

			/* signal the DVDNAV state machine to continue */
			pthread_mutex_lock(&waiter->mutex);
			pthread_cond_signal(&waiter->cond);
			pthread_mutex_unlock(&waiter->mutex);
			break;
		}
		case AVBOX_PLAYERCTL_DVD_SPU_STREAM_CHANGE:
		{
			struct avbox_player_waiter * const waiter = ctlmsg->data;

			DEBUG_PRINT(LOG_MODULE, "AVBOX_PLAYERCTL_DVD_SPU_STREAM_CHANGE");

			/* signal the DVDNAV state machine to continue */
			pthread_mutex_lock(&waiter->mutex);
			pthread_cond_signal(&waiter->cond);
			pthread_mutex_unlock(&waiter->mutex);
			break;
		}
		case AVBOX_PLAYERCTL_DVD_HIGHLIGHT:
		{
			struct avbox_player_waiter * const waiter = ctlmsg->data;
			dvdnav_highlight_event_t * const event = waiter->data;
			DEBUG_VPRINT(LOG_MODULE, "Hightlight button: %i",
				event->buttonN);
			/* signal the DVDNAV state machine to continue */
			pthread_mutex_lock(&waiter->mutex);
			pthread_cond_signal(&waiter->cond);
			pthread_mutex_unlock(&waiter->mutex);
			break;
		}
		case AVBOX_PLAYERCTL_DVD_NAV_PACKET:
		{
			struct avbox_player_waiter * const waiter = ctlmsg->data;

			avbox_player_process_menus(inst);
			inst->state_info.pos = (dvdnav_get_current_time(
				avbox_dvdio_dvdnav(inst->dvdio)) / (90L * 1000L)) * 1000L * 1000L;

			/* signal the DVDNAV state machine to continue */
			pthread_mutex_lock(&waiter->mutex);
			pthread_cond_signal(&waiter->cond);
			pthread_mutex_unlock(&waiter->mutex);
			break;
		}
#endif
		default:
			DEBUG_VABORT("player", "Invalid message type: %i", ctlmsg->id);
			abort();
		}
		free(ctlmsg);
		return AVBOX_DISPATCH_OK;
	}
#ifdef  ENABLE_DVD
	case AVBOX_MESSAGETYPE_INPUT:
	{
		struct avbox_input_message *event =
			avbox_message_payload(msg);

		DEBUG_PRINT("shell", "Input event received");

		switch (event->msg) {
		case MBI_EVENT_CONTEXT:
		{
			if (inst->dvdio != NULL) {
				DEBUG_PRINT(LOG_MODULE, "Menu pressed. Activating.");
				dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
				dvdnav_menu_call(dvdnav, DVD_MENU_Root);
			}
			break;
		}
		case MBI_EVENT_ENTER:
		{
			if (inst->dvdio != NULL) {
				DEBUG_PRINT(LOG_MODULE, "Enter pressed. Activating.");
				dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
				dvdnav_button_activate(dvdnav, dvdnav_get_current_nav_pci(dvdnav));
			}
			break;
		}
		case MBI_EVENT_BACK:
		{
			if (inst->dvdio != NULL) {
				dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
				ASSERT(dvdnav != NULL);

				/* if we're in a menu go up one level */
				if (!dvdnav_is_domain_vts(dvdnav) && !dvdnav_is_domain_fp(dvdnav)) {
					DEBUG_PRINT(LOG_MODULE, "BACK pressed. Going one level up.");
					dvdnav_go_up(avbox_dvdio_dvdnav(inst->dvdio));
				}

				/* let the shell process the event */
				return AVBOX_DISPATCH_CONTINUE;
			}
			break;
		}
		case MBI_EVENT_ARROW_UP:
		{
			if (inst->dvdio != NULL) {
				dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
				dvdnav_upper_button_select(dvdnav, dvdnav_get_current_nav_pci(dvdnav));
			}
			break;
		}
		case MBI_EVENT_ARROW_DOWN:
		{
			if (inst->dvdio != NULL) {
				dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
				dvdnav_lower_button_select(dvdnav, dvdnav_get_current_nav_pci(dvdnav));
			}
			break;
		}
		case MBI_EVENT_ARROW_LEFT:
		{
			if (inst->dvdio != NULL) {
				dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
				dvdnav_left_button_select(dvdnav, dvdnav_get_current_nav_pci(dvdnav));
			}
			break;
		}
		case MBI_EVENT_ARROW_RIGHT:
		{
			if (inst->dvdio != NULL) {
				dvdnav_t * const dvdnav = avbox_dvdio_dvdnav(inst->dvdio);
				dvdnav_right_button_select(dvdnav, dvdnav_get_current_nav_pci(dvdnav));
			}
			break;
		}
		default:
			return AVBOX_DISPATCH_CONTINUE;
		}
		avbox_input_eventfree(event);
		return AVBOX_DISPATCH_OK;
	}
#endif
	case AVBOX_MESSAGETYPE_TIMER:
	{
		struct avbox_timer_data * const timer_data =
			avbox_message_payload(msg);
		if (timer_data->id == inst->underrun_timer_id) {
			if (avbox_player_isunderrun(inst)) {
				avbox_player_handle_underrun(inst);
			} else {
				inst->underrun_timer_id = -1;
				avbox_player_doresume(inst);
				avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);
				DEBUG_PRINT(LOG_MODULE, "Underrun cleared");
			}

		} else if (timer_data->id == inst->stream_exit_timer_id) {
			avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STREAM_EXIT, NULL);
			inst->stream_exit_timer_id = -1;

		} else {
			LOG_VPRINT_ERROR("Unexpected timer: %i", timer_data->id);
		}
		free(timer_data);
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		avbox_dispatch_close();
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		return AVBOX_DISPATCH_OK;
	}
	default:
		DEBUG_VABORT("player", "Inavlid message received: %i", msgid);
		abort();
	}
}


/**
 * Run the player's control thread.
 */
static void *
avbox_player_run(void *arg)
{
	int quit = 0;
	struct avbox_player * const inst = (struct avbox_player*) arg;
	struct avbox_message * msg;

	DEBUG_SET_THREAD_NAME("player");
	DEBUG_PRINT("player", "Starting player control loop");

	if (avbox_dispatch_init() == -1) {
		LOG_VPRINT_ERROR("Could not initialize message dispatcher: %s",
			strerror(errno));
		return NULL;
	}

	inst->control_object = avbox_object_new(avbox_player_control, inst);
	if (inst->control_object == NULL) {
		LOG_VPRINT_ERROR("Could not create dispatch object: %s",
			strerror(errno));
		avbox_dispatch_shutdown();
		return NULL;
	}

	while (!quit) {
		if ((msg = avbox_dispatch_getmsg()) == NULL) {
			switch (errno) {
			case EAGAIN: continue;
			case ESHUTDOWN:
				quit = 1;
				continue;
			default:
				DEBUG_VABORT("player", "getmsg() returned %i: %s",
					errno, strerror(errno));
			}
		}
		avbox_message_dispatch(msg);
	}

	avbox_dispatch_shutdown();

	return NULL;
}


/****** BEGIN PUBLIC INTERFACE *******/


/**
 * Get the player status
 */
enum avbox_player_status
avbox_player_getstatus(struct avbox_player *inst)
{
	return inst->status;
}


/**
 * Seek.
 */
void
avbox_player_seek(struct avbox_player *inst, int flags, int64_t pos)
{
	struct avbox_player_seekargs *args;
	if ((args = malloc(sizeof(struct avbox_player_seekargs))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate memory for arguments");
		return;
	}
	args->flags = flags;
	args->pos = pos;
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_SEEK, args);
}


unsigned int
avbox_player_bufferstate(struct avbox_player *inst)
{
	ASSERT(inst != NULL);
	return inst->stream_percent;
}


char *
avbox_player_getmediafile(struct avbox_player *inst)
{
	ASSERT(inst != NULL);
	if (inst->media_file == NULL) {
		return NULL;
	} else {
		return strdup(inst->media_file);
	}
}


/**
 * Gets the title of the currently playing
 * media file or NULL if nothing is playing. The result needs to be
 * freed with free().
 */
char *
avbox_player_gettitle(struct avbox_player * const inst)
{
	char *title;
	pthread_mutex_lock(&inst->state_lock);
	title = strdup(inst->state_info.title);
	pthread_mutex_unlock(&inst->state_lock);
	return title;
}


/**
 * If path is not NULL it opens the file
 * specified by path and starts playing it. If path is NULL
 * it resumes playback if we're on the PAUSED state and return
 * failure code (-1) if we're on any other state.
 */
void
avbox_player_play(struct avbox_player *inst, const char * const path)
{
	char * path_copy = NULL;
	if (path != NULL && (path_copy = strdup(path)) == NULL) {
		LOG_PRINT_ERROR("Could not allocate copy of play path!");
		return;
	}
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_PLAY, path_copy);
}


/**
 * Plays a playlist.
 */
int
avbox_player_playlist(struct avbox_player* inst, LIST *playlist, struct avbox_playlist_item* selected_item)
{
	struct avbox_playlist_item *item, *item_copy;

	/* if our local list is not empty then free it first */
	if (!LIST_EMPTY(&inst->playlist)) {
		avbox_player_freeplaylist(inst);
	}

	/* copy the playlist */
	LIST_FOREACH(struct avbox_playlist_item*, item, playlist) {
		if ((item_copy = malloc(sizeof(struct avbox_playlist_item))) == NULL) {
			avbox_player_freeplaylist(inst);
			errno = ENOMEM;
			return -1;
		}
		if ((item_copy->filepath = strdup(item->filepath)) == NULL) {
			avbox_player_freeplaylist(inst);
			errno = ENOMEM;
			return -1;
		}

		LIST_ADD(&inst->playlist, item_copy);

		if (item == selected_item) {
			inst->playlist_item = item_copy;
		}
	}

	/* play the selected item */
	avbox_player_play(inst, inst->playlist_item->filepath);

	return 0;
}


/**
 * Pause the stream.
 */
void
avbox_player_pause(struct avbox_player* inst)
{
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_PAUSE, NULL);
}


/**
 * Stop playback.
 */
void
avbox_player_stop(struct avbox_player* inst)
{
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STOP, NULL);
}


/**
 * Subscribe to receive player notifications.
 */
int
avbox_player_subscribe(struct avbox_player * const inst,
	struct avbox_object * const object)
{
	struct avbox_player_subscriber *subscriber;
	if ((subscriber = avbox_player_findsubscriber(inst, object)) != NULL) {
		errno = EEXIST;
		return -1;
	}
	if ((subscriber = malloc(sizeof(struct avbox_player_subscriber))) == NULL) {
		ASSERT(errno == ENOMEM);
		return -1;
	}
	subscriber->object = object;
	LIST_APPEND(&inst->subscribers, subscriber);
	return 0;
}


/**
 * Unsubscribe from player events.
 */
int
avbox_player_unsubscribe(struct avbox_player * const inst,
	struct avbox_object * const object)
{
	struct avbox_player_subscriber *subscriber;
	if ((subscriber = avbox_player_findsubscriber(inst, object)) == NULL) {
		errno = ENOENT;
		return -1;
	}
	LIST_REMOVE(subscriber);
	free(subscriber);
	return 0;
}


/**
 * Get the media duration.
 */
void
avbox_player_getduration(struct avbox_player * const inst, int64_t *duration)
{
	ASSERT(inst != NULL);
	ASSERT(duration != NULL);
	*duration = inst->state_info.duration;
}


/**
 * Get the media position in microseconds.
 */
void
avbox_player_gettime(struct avbox_player * const inst, int64_t *time)
{
	ASSERT(inst != NULL);
	ASSERT(time != NULL);
	*time = inst->state_info.pos;
}


/**
 * Handle player messages.
 */
static int
avbox_player_handler(void * const context, struct avbox_message * const msg)
{
	struct avbox_player * const inst = context;
	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		ASSERT(inst != NULL);
		DEBUG_PRINT("player", "Destroying player");
#ifndef NDEBUG
		/* display a warning if there are any subscribers left */
		int cnt;
		LIST_COUNT(&inst->subscribers, cnt);
		if (cnt > 0) {
			DEBUG_VPRINT("player", "LEAK: There are still %d subscribers!",
				cnt);
		}
#endif

		/* TODO: We need to put the STOP logic on a
		 * static function and invoke it from there
		 * (and the STOP handler) */
		avbox_player_stop(inst);

		avbox_player_freeplaylist(inst);

		if (inst->media_file != NULL) {
			free((void*) inst->media_file);
		}

		/* destroy the dispatch object and quit the
		 * control thread */
		avbox_object_destroy(inst->control_object);
		pthread_join(inst->control_thread, NULL);

		break;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		DEBUG_PRINT("player", "Cleaning up after player");
		ASSERT(inst != NULL);
		free(inst);
		break;
	}
	default:
		DEBUG_VABORT("player", "Invalid message (type=%d)",
			avbox_message_id(msg));
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Gets the underlying dispatch object.
 */
struct avbox_object *
avbox_player_object(struct avbox_player * const inst)
{
	ASSERT(inst != NULL);
	return inst->object;
}


/**
 * Create a new player object.
 */
struct avbox_player*
avbox_player_new(struct avbox_window *window)
{
	struct avbox_player* inst;
	static int initialized = 0;

	/* initialize libav */
	if (!initialized) {
		av_register_all();
		avfilter_register_all();
		initialized = 1;
	}

	/* allocate memory for the player object */
	inst = malloc(sizeof(struct avbox_player));
	if (inst == NULL) {
		LOG_PRINT_ERROR("Cannot create new player instance. Out of memory");
		return NULL;
	}

	memset(inst, 0, sizeof(struct avbox_player));

	/* if no window argument was provided then use the root window */
	if (window == NULL) {
		window = avbox_video_getrootwindow(0);
		if (window == NULL) {
			LOG_PRINT_ERROR("Could not get root window");
			free(inst);
			return NULL;
		}
	}

	if ((inst->video_time = avbox_stopwatch_new()) == NULL) {
		LOG_PRINT_ERROR("Could not create stopwatch. Out of memory");
		return NULL;
	}

	/* create a dispatch object */
	if ((inst->object = avbox_object_new(avbox_player_handler, inst)) == NULL) {
		LOG_PRINT_ERROR("Could not create dispatch object");
		free(inst);
		return NULL;
	}

	inst->window = window;
	inst->video_stream_index = -1;
	inst->audio_stream_index = -1;
	inst->underrun_timer_id = -1;
	inst->stream_exit_timer_id = -1;
	inst->status = MB_PLAYER_STATUS_READY;
	inst->aspect_ratio.num = 16;
	inst->aspect_ratio.den = 9;
	inst->state_info.title = strdup("NONE");

	if (inst->state_info.title == NULL) {
		avbox_object_destroy(inst->object);
		free(inst);
		return NULL;
	}

	LIST_INIT(&inst->playlist);
	LIST_INIT(&inst->subscribers);

	/* initialize pthreads primitives */
	if (pthread_mutex_init(&inst->state_lock, NULL) != 0) {
		LOG_PRINT_ERROR("Cannot create player instance. Pthreads error");
		avbox_object_destroy(inst->object);
		free(inst->state_info.title);
		free(inst);
		return NULL;
	}

	/* initialize checkpoints */
	avbox_checkpoint_init(&inst->video_output_checkpoint);
	avbox_checkpoint_init(&inst->video_decoder_checkpoint);
	avbox_checkpoint_init(&inst->audio_decoder_checkpoint);
	avbox_checkpoint_init(&inst->stream_parser_checkpoint);

	/* fire control thread */
	if (pthread_create(&inst->control_thread, NULL, avbox_player_run, inst) != 0) {
		LOG_PRINT_ERROR("Could not create control thread");
		free(inst);
		return NULL;
	}

	return inst;
}
