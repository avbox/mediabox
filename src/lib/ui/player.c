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


/* This is the # of frames to decode ahead of time */
#define MB_VIDEO_BUFFER_FRAMES  (10)
#define MB_VIDEO_BUFFER_PACKETS (1)
#define MB_AUDIO_BUFFER_PACKETS (1)

#define ALIGNED(addr, bytes) \
    (((uintptr_t)(const void *)(addr)) % (bytes) == 0)


#define AVBOX_PLAYERCTL_PLAY		(0x01)
#define AVBOX_PLAYERCTL_PAUSE		(0x02)
#define AVBOX_PLAYERCTL_STOP		(0x03)
#define AVBOX_PLAYERCTL_SEEK		(0x04)
#define AVBOX_PLAYERCTL_THREADEXIT	(0x05)
#define AVBOX_PLAYERCTL_STREAM_READY	(0x06)
#define AVBOX_PLAYERCTL_AUDIODEC_READY	(0x07)
#define AVBOX_PLAYERCTL_VIDEODEC_READY	(0x08)
#define AVBOX_PLAYERCTL_AUDIOOUT_READY	(0x09)
#define AVBOX_PLAYERCTL_VIDEOOUT_READY 	(0x0A)
#define AVBOX_PLAYERCTL_STREAM_EXIT	(0x0B)
#define AVBOX_PLAYERCTL_BUFFER_UNDERRUN	(0x0C)

#define AVBOX_PLAYER_SEEK_ABSOLUTE	(0x01)
#define AVBOX_PLAYER_SEEK_CHAPTER	(0x02)
#define AVBOX_PLAYER_SEEK_RELATIVE	(0x04)


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


/**
 * Player structure.
 */
struct avbox_player
{
	struct avbox_window *window;
	struct avbox_window *video_window;
	struct avbox_object *object;
	struct avbox_object *control_object;
	struct avbox_queue *video_packets_q;
	struct avbox_queue *audio_packets_q;
	struct avbox_queue *video_frames_q;
	struct avbox_audiostream *audio_stream;
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
	int halting;
	int stream_quit;
	int gotpacket;
	int video_paused;
	int stream_percent;
	int stream_exiting;

	/* i don't think these are needed anymore */
	int audio_time_set;

	int64_t video_decoder_pts;
	int64_t lasttime;
	int64_t systemtimeoffset;
	int64_t (*getmastertime)(struct avbox_player *inst);

	avbox_checkpoint_t video_decoder_checkpoint;
	avbox_checkpoint_t video_output_checkpoint;
	avbox_checkpoint_t audio_decoder_checkpoint;
	avbox_checkpoint_t stream_parser_checkpoint;
	pthread_t video_decoder_thread;
	pthread_t video_output_thread;
	pthread_t audio_decoder_thread;
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
		/* first scale to fit to resolution and then
		 * adjust to aspect ratio */
		out->w = screen.w * SCALE;
		out->h = (((in.h * SCALE) * ((out->w * 100) / (in.w * SCALE))) / 100);
		out->h += (out->h * ((((screen.h * SCALE) - (((screen.w * SCALE) * inst->aspect_ratio.den)
			/ inst->aspect_ratio.num)) * 100) / (screen.h * SCALE))) / 100;
	} else {
		/* first scale to fit to resolution and then
		 * adjust to aspect ratio */
		out->h = screen.h * SCALE;
		out->w = (((in.w * SCALE) * ((out->h * 100) / (in.h * SCALE))) / 100);
		out->w += (out->w * ((((screen.w * SCALE) - (((screen.h * SCALE) * inst->aspect_ratio.den)
			/ inst->aspect_ratio.num)) * 100) / (screen.w * SCALE))) / 100;
	}

	/* scale result */
	out->w /= SCALE;
	out->h /= SCALE;

	ASSERT(out->w <= screen.w);
	ASSERT(out->h <= screen.h);
#undef SCALE
}


/**
 * Dump all video frames up to the specified
 * pts (in usecs)
 *
 * WARNING: DO NOT call this function from any thread except the
 * video output thread.
 */
static inline int
avbox_player_dumpvideo(struct avbox_player * const inst, const int64_t pts)
{
	int ret = 0, c = 0;
	int64_t video_time;
	AVFrame *frame;

	DEBUG_VPRINT("player", "Skipping frames until %li", pts);

	video_time = pts - 10000 - 1;

	while (video_time < (pts - 10000)) {

		/* first drain the decoded frames buffer */
		if ((frame = avbox_queue_peek(inst->video_frames_q, 1)) == NULL) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == ESHUTDOWN) {
				break;
			} else {
				LOG_VPRINT_ERROR("ERROR: avbox_queue_get() returned error: %s",
					strerror(errno));
				abort();
			}
		}

		video_time = av_rescale_q(frame->pts,
			inst->fmt_ctx->streams[inst->video_stream_index]->time_base,
			AV_TIME_BASE_Q);
		if (pts != -1 && video_time >= (pts - 10000)) {
			goto end;
		}

		/* dequeue the frame */
		if (avbox_queue_get(inst->video_frames_q) != frame) {
			LOG_PRINT_ERROR("We peeked one frame but got a different one. WTF?");
			abort();
		}
		av_frame_unref(frame);
		av_free(frame);
		c++;
		ret = 1;
	}
end:

	DEBUG_VPRINT("player", "Skipped %i frames", c);

	return ret;
}


/**
 * Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
static int64_t
avbox_player_getaudiotime(struct avbox_player * const inst)
{
	assert(inst->audio_stream != NULL);
	return inst->lasttime = avbox_audiostream_gettime(inst->audio_stream);
}


static void
avbox_player_resetsystemtime(struct avbox_player *inst, int64_t upts)
{
	(void) clock_gettime(CLOCK_MONOTONIC, &inst->systemreftime);
	inst->systemtimeoffset = upts;
}


static int64_t
avbox_player_getsystemtime(struct avbox_player *inst)
{
	struct timespec tv;
	if (UNLIKELY(inst->video_paused)) {
		return inst->lasttime;
	}
	(void) clock_gettime(CLOCK_MONOTONIC, &tv);
	return (inst->lasttime = (utimediff(&tv, &inst->systemreftime) + inst->systemtimeoffset));
}


/**
 * Update the display from main thread.
 */
static void *
avbox_player_doupdate(void *arg)
{
	struct avbox_player * const inst = (struct avbox_player*) arg;
	avbox_window_blit(inst->window, inst->video_window, MBV_BLITFLAGS_NONE, 0, 0);
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
	int64_t delay, frame_time = 0;
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

	avbox_window_setbgcolor(inst->video_window, AVBOX_COLOR(0x000000ff));
	avbox_window_clear(inst->video_window);

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

	if (inst->audio_stream_index == -1) {
		/* save the reference timestamp */
		avbox_player_resetsystemtime(inst, 0);
	}

	avbox_checkpoint_enable(&inst->video_output_checkpoint);

	/* signal control thread that we're ready */
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_VIDEOOUT_READY, NULL);

	DEBUG_PRINT("player", "Video renderer ready");

	while (1) {

		avbox_checkpoint_here(&inst->video_output_checkpoint);

		/* if the queue is empty signal the control thread */
		if (UNLIKELY(avbox_queue_count(inst->video_frames_q) == 0)) {
			avbox_player_sendctl(inst, AVBOX_PLAYERCTL_BUFFER_UNDERRUN, NULL);
		}

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

		/* copy the frame to the video window. For now we
		 * just scale here but in the future this should be done
		 * by the video driver (possibly accelerated). */
		if ((buf = avbox_window_lock(inst->video_window, MBV_LOCKFLAGS_WRITE, &pitch)) == NULL) {
			LOG_VPRINT_ERROR("Could not lock video window: %s", strerror(errno));
		} else {
			ASSERT(ALIGNED(*frame->data, 16));
			ASSERT(ALIGNED(buf, 16));
			buf += pitch * ((target_height - inst->video_size.h) / 2);
			sws_scale(swscale_ctx, (uint8_t const * const *) frame->data,
				&linesize, 0, height, &buf, &pitch);
			avbox_window_unlock(inst->video_window);
		}

		/* get the frame pts */
		if  (LIKELY(frame->pts != AV_NOPTS_VALUE)) {
			int64_t elapsed;

			frame_time = av_frame_get_best_effort_timestamp(frame);
			frame_time = av_rescale_q(frame_time,
				inst->fmt_ctx->streams[inst->video_stream_index]->time_base,
				AV_TIME_BASE_Q);
			elapsed = inst->getmastertime(inst);

			if (UNLIKELY(elapsed > frame_time)) {
				delay = 0;
				if (elapsed - frame_time > 100000) {
					/* if the decoder is lagging behind skip a few frames */
					if (UNLIKELY(avbox_player_dumpvideo(inst, elapsed))) {
						continue;
					}

					/* skip just this frame */
					goto frame_complete;
				}
			} else {
				delay = frame_time - elapsed;
			}

			if (LIKELY((delay & ~0xFF) > 0)) {
				usleep(delay);
				continue;
			}
		}

		/* perform the actual update from the main thread */
		if ((del = avbox_application_delegate(avbox_player_doupdate, inst)) == NULL) {
			LOG_PRINT_ERROR("Could not delegate update!");
		} else {
			avbox_delegate_wait(del, NULL);
		}
frame_complete:
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

	return NULL;
}


/**
 * Decodes video frames in the background.
 */
static void *
avbox_player_video_decode(void *arg)
{
	int ret;
	struct avbox_player *inst = (struct avbox_player*) arg;
	char video_filters[512];
	AVPacket *packet;
	AVFrame *video_frame_nat = NULL, *video_frame_flt = NULL;
	AVFilterGraph *video_filter_graph = NULL;
	AVFilterContext *video_buffersink_ctx = NULL;
	AVFilterContext *video_buffersrc_ctx = NULL;

	DEBUG_SET_THREAD_NAME("video_decode");
	DEBUG_PRINT("player", "Video decoder starting");

	ASSERT(inst != NULL);
	ASSERT(inst->fmt_ctx != NULL);
	ASSERT(inst->video_decoder_pts == 0);
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

	while (1) {

		avbox_checkpoint_here(&inst->video_decoder_checkpoint);

		/* get next packet from queue */
		if ((packet = avbox_queue_peek(inst->video_packets_q, 1)) == NULL) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == ESHUTDOWN) {
				break;
			}
			LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
				strerror(errno));
			break;
		}

		//DEBUG_VPRINT("player", "Video dts: %li (pts=%li)", packet->dts, packet->pts);

		/* send packet to codec for decoding */
		if (UNLIKELY((ret = avcodec_send_packet(inst->video_codec_ctx, packet)) < 0)) {
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				/* fall through */
			} else if (ret == AVERROR_INVALIDDATA) {
				LOG_PRINT_ERROR("Invalid data sent to video decoder");
				ret = 0; /* so we still dequeue it */

			} else {
				char err[256];
				av_strerror(ret, err, sizeof(err));
				LOG_VPRINT_ERROR("Error decoding video packet (%i): %s",
					ret, err);
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT,
					&inst->audio_decoder_thread);
				goto decoder_exit;
			}
		}
		if (ret == 0){
			if (avbox_queue_get(inst->video_packets_q) != packet) {
				LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result: %s",
					strerror(errno));
				goto decoder_exit;
			}
			/* free packet */
			av_packet_unref(packet);
			free(packet);
		}

		/* read decoded frames from codec */
		while (LIKELY((ret = avcodec_receive_frame(inst->video_codec_ctx, video_frame_nat))) == 0) {

			if (video_frame_nat->pkt_dts == AV_NOPTS_VALUE) {
				video_frame_nat->pts = 0;
			} else {
				video_frame_nat->pts = video_frame_nat->pkt_dts;
			}

			/* push the decoded frame into the filtergraph */
			if (UNLIKELY(av_buffersrc_add_frame_flags(video_buffersrc_ctx,
				video_frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)) {
				LOG_PRINT_ERROR("Error feeding video filtergraph");
				goto decoder_exit;
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

				/* update the video decoder pts */
				inst->video_decoder_pts = video_frame_flt->pts;

				/* add frame to decoded frames queue */
				while (1) {

					/* since we may get stuck here if the frames queue
					 * is full we need a way to break off when we're
					 * suspending. Therefore a side effect of suspending
					 * is that a single frame may get lost */
					if (inst->halting) {
						av_frame_unref(video_frame_flt);
						av_free(video_frame_flt);
						video_frame_flt = NULL;
						break;
					}

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
		if (ret != 0 && ret != AVERROR(EAGAIN)) {
			LOG_VPRINT_ERROR("ERROR: avcodec_receive_frame() returned %d (video)",
				ret);
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
			if (ret != AVERROR_EOF) {
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
	int ret;
	struct avbox_player * const inst = (struct avbox_player * const) arg;
	const char *audio_filters ="aresample=48000,aformat=sample_fmts=s16:channel_layouts=stereo";
	AVFrame *audio_frame_nat = NULL;
	AVFrame *audio_frame = NULL;
	AVFilterGraph *audio_filter_graph = NULL;
	AVFilterContext *audio_buffersink_ctx = NULL;
	AVFilterContext *audio_buffersrc_ctx = NULL;
	AVPacket *packet;

	MB_DEBUG_SET_THREAD_NAME("audio_decoder");

	ASSERT(inst != NULL);
	ASSERT(inst->fmt_ctx != NULL);
	ASSERT(inst->audio_codec_ctx == NULL);
	ASSERT(inst->audio_time_set == 0);
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
		audio_filters, inst->audio_stream_index) < 0) {
		LOG_PRINT_ERROR("Could not init filter graph!");
		goto end;
	}

	avbox_checkpoint_enable(&inst->audio_decoder_checkpoint);

	/* signl control thread that we're ready */
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_AUDIODEC_READY, NULL);

	DEBUG_PRINT("player", "Audio decoder ready");

	while (1) {

		avbox_checkpoint_here(&inst->audio_decoder_checkpoint);

		/* wait for the stream decoder to give us some packets */
		if ((packet = avbox_queue_peek(inst->audio_packets_q, 1)) == NULL) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == ESHUTDOWN) {
				break;
			}
			LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
				strerror(errno));
			avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT,
				&inst->audio_decoder_thread);
			goto end;
		}

		/* send packets to codec for decoding */
		if ((ret = avcodec_send_packet(inst->audio_codec_ctx, packet)) < 0) {
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
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
					&inst->audio_decoder_thread);
				goto end;
			}
		}
		if (ret == 0) {
			/* remove packet from queue */
			if (avbox_queue_get(inst->audio_packets_q) != packet) {
				LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result (%p): %s",
					packet, strerror(errno));
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT,
					&inst->audio_decoder_thread);
				goto end;
			}
			/* free packet */
			av_packet_unref(packet);
			free(packet);
		}

		/* read decoded frames from codec */
		while ((ret = avcodec_receive_frame(inst->audio_codec_ctx, audio_frame_nat)) == 0) {
			/* push the audio data from decoded frame into the filtergraph */
			if (UNLIKELY(av_buffersrc_add_frame_flags(audio_buffersrc_ctx,
				audio_frame_nat, 0) < 0)) {
				LOG_PRINT_ERROR("Error while feeding the audio filtergraph");
				break;
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
						&inst->audio_decoder_thread);

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
					DEBUG_VPRINT("player", "First audio pts: %li unscaled=%li",
						pts, audio_frame->pts);
					inst->audio_time_set = 1;
				}

				/* write frame to audio stream and free it */
				avbox_audiostream_write(inst->audio_stream, audio_frame->data[0],
					audio_frame->nb_samples);
				av_frame_unref(audio_frame);
			}
		}
		if (ret != 0 && ret != AVERROR(EAGAIN)) {
			LOG_VPRINT_ERROR("ERROR!: avcodec_receive_frame() returned %d (audio)",
				AVERROR(ret));
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
		if (ret != AVERROR_EOF) {
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

	MB_DEBUG_SET_THREAD_NAME("stream_parser");

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

	inst->video_paused = 0;
	inst->lasttime = 0;

	DEBUG_VPRINT("player", "Attempting to play '%s'", inst->media_file);

	/* open file */
	av_dict_set(&stream_opts, "timeout", "30000000", 0);
	if (avformat_open_input(&inst->fmt_ctx, inst->media_file, NULL, &stream_opts) != 0) {
		LOG_VPRINT_ERROR("Could not open stream '%s'",
			inst->media_file);
		goto decoder_exit;
	}

	if (avformat_find_stream_info(inst->fmt_ctx, NULL) < 0) {
		LOG_PRINT_ERROR("Could not find stream info!");
		goto decoder_exit;
	}

	/* if there's an audio stream start the audio decoder */
	if ((inst->audio_stream_index =
		av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0)) >= 0) {

		DEBUG_PRINT("player", "Audio stream found");

		/* allocate filtered audio frames */
		inst->getmastertime = avbox_player_getaudiotime; /* video is slave to audio */

		/* create audio stream */
		if ((inst->audio_stream = avbox_audiostream_new()) == NULL) {
			goto decoder_exit;
		}

		if ((inst->audio_packets_q = avbox_queue_new(MB_AUDIO_BUFFER_PACKETS)) == NULL) {
			LOG_VPRINT_ERROR("Could not create audio packets queue: %s!",
				strerror(errno));
			goto decoder_exit;
		}
	}

	/* if the file contains a video stream fire the video decoder */
	if ((inst->video_stream_index =
		av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0)) >= 0) {
		inst->video_decoder_pts = 0;
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
		if ((inst->video_frames_q = avbox_queue_new(MB_VIDEO_BUFFER_FRAMES)) == NULL) {
			LOG_VPRINT_ERROR("Could not create frames queue: %s!",
				strerror(errno));
			goto decoder_exit;
		}

		DEBUG_VPRINT("player", "Video stream %i selected",
			inst->video_stream_index);
	}

	/* if there's no streams to decode then exit */
	if (inst->audio_stream_index == -1 && inst->video_stream_index == -1) {
		LOG_PRINT_ERROR("No streams to decode!");
		goto decoder_exit;
	}

	/* make sure that all queues are empty */
	assert(avbox_queue_count(inst->audio_packets_q) == 0);
	assert(avbox_queue_count(inst->video_packets_q) == 0);
	assert(avbox_queue_count(inst->video_frames_q) == 0);

	/* enable checkpoint */
	avbox_checkpoint_enable(&inst->stream_parser_checkpoint);

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

	pthread_mutex_lock(&inst->state_lock);
	inst->stream_exiting = 0;
	pthread_mutex_unlock(&inst->state_lock);

	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STREAM_EXIT,
		&inst->stream_thread);

	DEBUG_PRINT("player", "Stream parser thread bailing out");

	return NULL;
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
	avbox_queue_wake(inst->audio_packets_q);
	avbox_queue_wake(inst->video_packets_q);
	avbox_checkpoint_wait(&inst->stream_parser_checkpoint);

	if (inst->audio_stream_index != -1) {
		avbox_checkpoint_halt(&inst->audio_decoder_checkpoint);
		avbox_queue_wake(inst->audio_packets_q);
		avbox_checkpoint_wait(&inst->audio_decoder_checkpoint);
		avbox_audiostream_pause(inst->audio_stream);
	}

	if (inst->video_stream_index != -1) {
		avbox_checkpoint_halt(&inst->video_decoder_checkpoint);
		inst->halting = 1;
		avbox_queue_wake(inst->video_packets_q);
		avbox_queue_wake(inst->video_frames_q);
		avbox_checkpoint_wait(&inst->video_decoder_checkpoint);
		inst->halting = 0;

		avbox_checkpoint_halt(&inst->video_output_checkpoint);
		avbox_queue_wake(inst->video_frames_q);
		avbox_checkpoint_wait(&inst->video_output_checkpoint);
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
		avbox_queue_wake(inst->video_frames_q);
		avbox_checkpoint_wait(&inst->video_output_checkpoint);
		inst->video_paused = 1;
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
		if (inst->video_paused) {
			inst->video_paused = 0;
			avbox_player_resetsystemtime(inst, inst->video_decoder_pts);
			avbox_checkpoint_continue(&inst->video_output_checkpoint);
		}
	}
}


/**
 * Stop playing stream.
 */
static void
avbox_player_dostop(struct avbox_player * const inst)
{
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
			AVPacket *packet;
			AVFrame *frame;

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
				avbox_player_resetsystemtime(inst, seek_to);
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
				avbox_audiostream_setclock(inst->audio_stream, seek_to);
				avcodec_flush_buffers(inst->audio_codec_ctx);
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
			ASSERT(inst->getmastertime(inst) == seek_to);

			DEBUG_VPRINT("player", "Seeking (newpos=%li)",
				inst->getmastertime(inst));

			/* flush stream buffers */
			avformat_flush(inst->fmt_ctx);

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
			if (avbox_queue_count(inst->video_frames_q) <
				MB_VIDEO_BUFFER_FRAMES - 2) {
				underrun = 1;
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
		const int wanted = MB_VIDEO_BUFFER_FRAMES;
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
				if (pthread_create(&inst->audio_decoder_thread, NULL, avbox_player_audio_decode, inst) != 0) {
					LOG_PRINT_ERROR("Could not create audio decoder thread!");
					abort();
				}
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

			if (inst->audio_stream_index != -1) {
				ASSERT(inst->audio_stream != NULL);
				if (avbox_audiostream_count(inst->audio_stream) > 0 ||
					avbox_queue_count(inst->audio_packets_q) > 0) {
					avbox_player_delay_stream_exit(inst);
					return AVBOX_DISPATCH_OK;
				}
				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_AUDIOOUT) {
					avbox_player_resetsystemtime(inst,
						avbox_audiostream_gettime(inst->audio_stream));
					inst->getmastertime = avbox_player_getsystemtime;
					avbox_audiostream_destroy(inst->audio_stream);
					inst->audio_stream = NULL;
				}
				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_AUDIODEC) {
					pthread_join(inst->audio_decoder_thread, NULL);
				}
				avbox_queue_destroy(inst->audio_packets_q);
				inst->audio_packets_q = NULL;
				inst->audio_stream_index = -1;
				inst->audio_time_set = 0;
			}

			ASSERT(inst->audio_stream == NULL);

			if (inst->video_stream_index != -1) {
				if (avbox_queue_count(inst->video_frames_q) > 0 ||
					avbox_queue_count(inst->video_packets_q) > 0) {
					avbox_player_delay_stream_exit(inst);
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

			/* join the stream thread */
			pthread_join(inst->stream_thread, NULL);

			/* clean other stuff */
			if (inst->fmt_ctx != NULL) {
				avformat_close_input(&inst->fmt_ctx);
				inst->fmt_ctx = NULL;
			}

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

			/* update status */
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
			avbox_player_doseek(inst, args->flags, args->pos);
			free(args);
			break;
		}
		case AVBOX_PLAYERCTL_BUFFER_UNDERRUN:
		{
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
		case AVBOX_PLAYERCTL_THREADEXIT:
		{
			LOG_PRINT_ERROR("Thread exitted unexpectedly!");
			inst->play_state = AVBOX_PLAYER_PLAYSTATE_STOPPING;
			avbox_player_dostop(inst);
			break;
		}
		default:
			DEBUG_VABORT("player", "Invalid message type: %i", ctlmsg->id);
			abort();
		}
		return AVBOX_DISPATCH_OK;
	}
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
 * Seek to a chapter.
 */
void
avbox_player_seek_chapter(struct avbox_player *inst, int incr)
{
	struct avbox_player_seekargs *args;
	if ((args = malloc(sizeof(struct avbox_player_seekargs))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate memory for arguments");
		return;
	}
	args->flags = AVBOX_PLAYER_SEEK_CHAPTER;
	args->pos = incr;
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
	return strdup(inst->media_file);
}


/**
 * Gets the title of the currently playing
 * media file or NULL if nothing is playing. The result needs to be
 * freed with free().
 */
char *
avbox_player_gettitle(struct avbox_player * const inst)
{
	char *ret = NULL;
	AVDictionaryEntry *title_entry;
	ASSERT(inst != NULL);
	pthread_mutex_lock(&inst->state_lock);

	if (inst->stream_exiting || inst->fmt_ctx == NULL || inst->fmt_ctx->metadata == NULL) {
		goto end;
	}
	if ((title_entry = av_dict_get(inst->fmt_ctx->metadata, "title", NULL, 0)) != NULL) {
		if (title_entry->value != NULL) {
			ret = strdup(title_entry->value);
			goto end;
		}
	}
	ret = strdup(inst->media_file);
end:
	pthread_mutex_unlock(&inst->state_lock);
	return ret;
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
	pthread_mutex_lock(&inst->state_lock);
	if (inst->stream_exiting || inst->fmt_ctx == NULL) {
		*duration = 0;
	} else {
		*duration = inst->fmt_ctx->duration;
	}
	pthread_mutex_unlock(&inst->state_lock);
}


/**
 * Get the media position in microseconds.
 */
void
avbox_player_gettime(struct avbox_player * const inst, int64_t *time)
{
	ASSERT(inst != NULL);
	ASSERT(time != NULL);
	pthread_mutex_lock(&inst->state_lock);
	if (inst->stream_exiting || inst->getmastertime == NULL) {
		*time = 0;
	} else {
		*time = inst->getmastertime(inst);
	}
	pthread_mutex_unlock(&inst->state_lock);

}


/**
 * Update the player window
 */
void
avbox_player_update(struct avbox_player *inst)
{
	if (inst->play_state == AVBOX_PLAYER_PLAYSTATE_PLAYING) {
		avbox_player_doupdate(inst);
	} else {
		LOG_PRINT_ERROR("avbox_player_update() called while not playing. Ignoring for now.");
	}
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

	LIST_INIT(&inst->playlist);
	LIST_INIT(&inst->subscribers);

	/* initialize pthreads primitives */
	if (pthread_mutex_init(&inst->state_lock, NULL) != 0) {
		LOG_PRINT_ERROR("Cannot create player instance. Pthreads error");
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
