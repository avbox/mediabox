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
#include <sched.h>
#ifdef HAVE_MALLOC_TRIM
#	include <malloc.h>
#endif

#define LOG_MODULE "player"

#include "../avbox.h"
#include "player_p.h"

#ifdef ENABLE_MMAL
#	include "mmaldecode.h"
#endif

/* Define to log missed deadlines */
/* #define DEBUG_LATENCY	(1) */

/* Be careful when changing this. */
#define MB_VIDEO_BUFFER_PACKETS (1)
#define MB_AUDIO_BUFFER_PACKETS (30)

#define AVBOX_LATENCY_CORRECT_MAX 	(0)
#define AVBOX_MIN_FRAME_WAIT_US		(5000LL)
#define AVBOX_SKIP_FRAME_THRESHOLD	(400LL * 1000LL)
#define AVBOX_SKIP_FRAME_MAX		(3)
#define AVBOX_BUFFER_MSECS		(300)
#define AVBOX_BUFFER_VIDEO		(30 / (1000 / decode_cache_size))
#define AVBOX_BUFFER_AUDIO		(48000 / (1000 / decode_cache_size))

/* initial sizes for the pools */
#define AVBOX_AVPACKET_POOL_SIZE	(MB_VIDEO_BUFFER_PACKETS + MB_AUDIO_BUFFER_PACKETS + 25)
#define AVBOX_AVFRAME_POOL_SIZE		((AVBOX_BUFFER_VIDEO * 2) + 25)
#define AVBOX_VIDEO_PACKET_POOL_SIZE	(AVBOX_BUFFER_VIDEO + 5)
#define AVBOX_CTLMSG_POOL_SIZE		(5)



#define ALIGNED(addr, bytes) \
    (((uintptr_t)(const void *)(addr)) % (bytes) == 0)


/* playback startup stages */
#define AVBOX_PLAYER_PLAYSTATE_READY		(0x00)
#define AVBOX_PLAYER_PLAYSTATE_STREAM		(0x01)
#define AVBOX_PLAYER_PLAYSTATE_AUDIODEC		(0x02)
#define AVBOX_PLAYER_PLAYSTATE_VIDEODEC		(0x03)
#define AVBOX_PLAYER_PLAYSTATE_AUDIOOUT		(0x04)
#define AVBOX_PLAYER_PLAYSTATE_VIDEOOUT		(0x05)
#define AVBOX_PLAYER_PLAYSTATE_PLAYING		(0x06)


LISTABLE_STRUCT(avbox_player_subscriber,
	struct avbox_object *object;
);


LISTABLE_STRUCT(avbox_player_ctlmsg,
	int id;
	void *data;
);


struct avbox_player_seekargs
{
	int64_t pos;
	int flags;
};


static int decode_cache_size = AVBOX_BUFFER_MSECS;


static struct avbox_av_packet*
acquire_av_packet(struct avbox_player * const inst)
{
	struct avbox_av_packet *packet;
	pthread_mutex_lock(&inst->av_packet_pool_lock);
	packet = LIST_TAIL(struct avbox_av_packet*, &inst->av_packet_pool);
	if (UNLIKELY(packet == NULL)) {
		if ((packet = malloc(sizeof(struct avbox_av_packet))) == NULL) {
			ASSERT(errno == ENOMEM);
			pthread_mutex_unlock(&inst->av_packet_pool_lock);
			return NULL;
		}
		if ((packet->avpacket = av_packet_alloc()) == NULL) {
			pthread_mutex_unlock(&inst->av_packet_pool_lock);
			free(packet);
			errno = ENOMEM;
			return NULL;
		}
#ifdef DEBUG_MEMORY_POOLS
		inst->av_packet_pool_allocs++;
		if (inst->pools_primed) {
			LOG_VPRINT_INFO("Allocated AVPacket (total_allocs=%i)!",
				inst->av_packet_pool_allocs);
		}
#endif
	} else {
		LIST_REMOVE(packet);
	}
	pthread_mutex_unlock(&inst->av_packet_pool_lock);
	return packet;
}


INTERNAL void
release_av_packet(struct avbox_player * const inst, struct avbox_av_packet * const packet)
{
	pthread_mutex_lock(&inst->av_packet_pool_lock);
	LIST_ADD(&inst->av_packet_pool, packet);
	pthread_mutex_unlock(&inst->av_packet_pool_lock);
}


INTERNAL struct avbox_av_frame*
acquire_av_frame(struct avbox_player * const inst)
{
	struct avbox_av_frame *frame;
	pthread_mutex_lock(&inst->frame_pool_lock);
	frame = LIST_TAIL(struct avbox_av_frame*, &inst->frame_pool);
	if (UNLIKELY(frame == NULL)) {
		if ((frame = malloc(sizeof(struct avbox_av_frame))) == NULL) {
			ASSERT(errno == ENOMEM);
			pthread_mutex_unlock(&inst->frame_pool_lock);
			return NULL;
		}
		if ((frame->avframe = av_frame_alloc()) == NULL) {
			pthread_mutex_unlock(&inst->frame_pool_lock);
			free(frame);
			errno = ENOMEM;
			return NULL;
		}
#ifdef DEBUG_MEMORY_POOLS
		inst->frame_pool_allocs++;
		if (inst->pools_primed) {
			LOG_VPRINT_INFO("Allocated AVFrame (total_allocs=%i)",
				inst->frame_pool_allocs);
		}
#endif
	} else {
		LIST_REMOVE(frame);
	}
	pthread_mutex_unlock(&inst->frame_pool_lock);
	return frame;
}


INTERNAL void
release_av_frame(struct avbox_player * const inst, struct avbox_av_frame * const frame)
{
	pthread_mutex_lock(&inst->frame_pool_lock);
	LIST_ADD(&inst->frame_pool, frame);
	pthread_mutex_unlock(&inst->frame_pool_lock);
}


INTERNAL struct avbox_player_packet*
acquire_packet(struct avbox_player * const inst)
{
	struct avbox_player_packet *packet;
	pthread_mutex_lock(&inst->packet_pool_lock);
	packet = LIST_TAIL(struct avbox_player_packet*, &inst->packet_pool);
	if (UNLIKELY(packet == NULL)) {
		if ((packet = malloc(sizeof(struct avbox_player_packet))) == NULL) {
			ASSERT(errno == ENOMEM);
			pthread_mutex_unlock(&inst->packet_pool_lock);
			return NULL;
		}
#ifdef DEBUG_MEMORY_POOLS
		inst->packet_pool_allocs++;
		if (inst->pools_primed) {
			LOG_VPRINT_INFO("Allocated video packet (total_allocs=%i)",
				inst->packet_pool_allocs);
		}
#endif
	} else {
		LIST_REMOVE(packet);
	}
	pthread_mutex_unlock(&inst->packet_pool_lock);
	return packet;
}


INTERNAL void
release_packet(struct avbox_player * const inst, struct avbox_player_packet * const packet)
{
	pthread_mutex_lock(&inst->packet_pool_lock);
	LIST_ADD(&inst->packet_pool, packet);
	pthread_mutex_unlock(&inst->packet_pool_lock);
}


static struct avbox_player_ctlmsg*
acquire_ctlmsg(struct avbox_player * const inst)
{
	struct avbox_player_ctlmsg *msg;
	pthread_mutex_lock(&inst->ctlmsg_pool_lock);
	msg = LIST_TAIL(struct avbox_player_ctlmsg*, &inst->ctlmsg_pool);
	if (UNLIKELY(msg == NULL)) {
		if (UNLIKELY((msg = malloc(sizeof(struct avbox_player_ctlmsg))) == NULL)) {
			ASSERT(errno == ENOMEM);
			pthread_mutex_unlock(&inst->ctlmsg_pool_lock);
			return NULL;
		}
#ifdef DEBUG_MEMORY_POOLS
		inst->ctlmsg_pool_allocs++;
		if (inst->pools_primed) {
			LOG_VPRINT_INFO("Allocated video packet (total_allocs=%i)",
				inst->ctlmsg_pool_allocs);
		}
#endif
	} else {
		LIST_REMOVE(msg);
	}
	pthread_mutex_unlock(&inst->ctlmsg_pool_lock);
	return msg;
}


static void
release_ctlmsg(struct avbox_player * const inst, struct avbox_player_ctlmsg * const msg)
{
	pthread_mutex_lock(&inst->ctlmsg_pool_lock);
	LIST_ADD(&inst->ctlmsg_pool, msg);
	pthread_mutex_unlock(&inst->ctlmsg_pool_lock);
}


static void
prime_pools(struct avbox_player * const inst)
{
	int i;
	void *pointers[1024];

	DEBUG_PRINT(LOG_MODULE, "Priming memory pools");

	/* prime AVFrame pool */
	for (i = 0; i < AVBOX_AVFRAME_POOL_SIZE; i++) {
		if ((pointers[i] = acquire_av_frame(inst)) == NULL) {
			ABORT("Ran out of memory while priming pools!");
		}
	}
	for (i = 0; i < AVBOX_AVFRAME_POOL_SIZE; i++) {
		release_av_frame(inst, pointers[i]);
	}

	/* prime AVPacket pool */
	for (i = 0; i < AVBOX_AVPACKET_POOL_SIZE; i++) {
		if ((pointers[i] = acquire_av_packet(inst)) == NULL) {
			ABORT("Ran out of memory while priming pools!");
		}
	}
	for (i = 0; i < AVBOX_AVPACKET_POOL_SIZE; i++) {
		release_av_packet(inst, pointers[i]);
	}

	/* prime video packet pool */
	for (i = 0; i < AVBOX_VIDEO_PACKET_POOL_SIZE; i++) {
		if ((pointers[i] = acquire_packet(inst)) == NULL) {
			ABORT("Ran out of memory while priming pools!");
		}
	}
	for (i = 0; i < AVBOX_VIDEO_PACKET_POOL_SIZE; i++) {
		release_packet(inst, pointers[i]);
	}

	/* priming the ctlmsg pool */
	for (i = 0; i < AVBOX_CTLMSG_POOL_SIZE; i++) {
		if ((pointers[i] = acquire_ctlmsg(inst)) == NULL) {
			ABORT("Ran out of memory while priming pools!");
		}
	}
	for (i = 0; i < AVBOX_CTLMSG_POOL_SIZE; i++) {
		release_ctlmsg(inst, pointers[i]);
	}

#ifdef DEBUG_MEMORY_POOLS
	inst->pools_primed = 1;
#endif
	DEBUG_PRINT(LOG_MODULE, "Memory pools primed");
}


/**
 * Sends a control message to the player.
 */
int
avbox_player_sendctl(struct avbox_player * const inst,
	const int ctl, void * const data)
{
	struct avbox_player_ctlmsg * msg;
	struct avbox_object * control_object =
		avbox_thread_object(inst->control_thread);

	if (UNLIKELY((msg = acquire_ctlmsg(inst)) == NULL)) {
		LOG_PRINT_ERROR("Could not send control message: Out of memory");
		return -1;
	}

	msg->id = ctl;
	msg->data = data;

	if (UNLIKELY(avbox_object_sendmsg(&control_object,
		AVBOX_MESSAGETYPE_USER, AVBOX_DISPATCH_UNICAST, msg) == NULL)) {
		LOG_VPRINT_ERROR("Could not send control message: %s",
			strerror(errno));
		release_ctlmsg(inst, msg);
		return -1;
	}
	return 0;
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
	in.w = inst->state_info.video_res.w;
	in.h = inst->state_info.video_res.h;

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
	int target_width, target_height;
	struct avbox_player * const inst = context;

	if (inst->video_window == NULL) {
		avbox_window_clear(window);
		return 0;
	}

	avbox_window_getcanvassize(inst->window, &target_width, &target_height);

	const int x = (target_width - inst->state_info.scaled_res.w) >> 1;
	const int y = (target_height - inst->state_info.scaled_res.h) >> 1;

	/* if the window has been damaged we need to
	 * paint the whole window */
	if (UNLIKELY(y > 0 && avbox_window_damaged(window))) {
		/* avbox_window_clear(window); */
		avbox_window_setbgcolor(inst->window, AVBOX_COLOR(0x000000ff));
		avbox_window_setcolor(inst->window, AVBOX_COLOR(0x000000ff));
		avbox_window_fillrectangle(inst->window,
			0, 0, target_width, y);
	}

	/* scale and show the frame */
	avbox_window_scaleblit(window, inst->video_window, MBV_BLITFLAGS_NONE,
		x, y, inst->state_info.scaled_res.w, inst->state_info.scaled_res.h);

#ifdef ENABLE_DVD
	if (UNLIKELY(inst->stream.self != NULL && inst->stream.highlight != NULL)) {
		struct avbox_rect *highlight;
		if ((highlight = inst->stream.highlight(inst->stream.self)) != NULL) {

			const int wpc = (100 * inst->state_info.scaled_res.w) / inst->state_info.video_res.w;
			const int hpc = (100 * inst->state_info.scaled_res.h) / inst->state_info.video_res.h;;
			const int wos = (target_width > inst->state_info.scaled_res.w) ? (target_width - inst->state_info.scaled_res.w) / 2 : 0;
			const int hos = (target_height > inst->state_info.scaled_res.h) ? (target_height - inst->state_info.scaled_res.h) / 2 : 0;

			avbox_window_fillrectangle(inst->window,
				((highlight->x * wpc) / 100) + wos, ((highlight->y * hpc) / 100) + hos,
				((highlight->w * wpc) / 100), ((highlight->h * hpc) / 100));
		}
	}
#endif

	return 0;
}


struct avbox_player_updateargs
{
	struct avbox_player *inst;
	struct avbox_av_frame *frame;
};


/**
 * Update the display from main thread.
 */
static void *
avbox_player_doupdate(void *arg)
{
	struct avbox_player_updateargs * const args = arg;
	struct avbox_player * const inst = args->inst;
	struct avbox_av_frame * const frame = args->frame;

	/* DEBUG_PRINT(LOG_MODULE, "Updating window"); */

	if (LIKELY(inst->video_window != NULL && frame != NULL)) {
#ifdef DEBUG_LATENCY
		const int64_t current_time = inst->getmastertime(inst);
		const int64_t frame_time = av_rescale_q(frame->avframe->pts,
			inst->state_info.time_base,
			AV_TIME_BASE_Q);
		if (current_time > (frame_time + (20LL * 1000LL))) {
			LOG_VPRINT_INFO("Frame presented too late (expected=%" PRIi64 " is=%" PRIi64 " diff=%" PRIi64 ")",
				frame_time, current_time, current_time - frame_time);
		}
#endif
		/* upload the frame to the window surface */
		avbox_window_blitbuf(inst->video_window,
			inst->state_info.pix_fmt,
			(void**) frame->avframe->data,
			frame->avframe->linesize,
			inst->state_info.video_res.w,
			inst->state_info.video_res.h,
			0, 0);

		avbox_window_update(inst->window);

		/* The GPU drivers may have another RT thread that needs
		 * to run in order for the frames to be shown so yield
		 * the CPU so it can do it right away */
		/* sched_yield(); */

		/* Just because avbox_window_blitbuf() returned doesn't mean
		 * that the blitting is complete. This is the case in an
		 * accelerated scenario where the frame is in GPU memory. In
		 * the case of MMAL this causes blinking (black frames whenever
		 * the blitting doesn't happen fast enough). So we delay the
		 * freeing of the frame until the next frame. */
		if (LIKELY(inst->last_video_frame != NULL)) {
			av_frame_unref(inst->last_video_frame->avframe);
			release_av_frame(inst, inst->last_video_frame);
		}
		inst->last_video_frame = frame;
	} else {
		avbox_window_update(inst->window);
	}

	return NULL;
}


static void *
avbox_player_createwindow(void *arg)
{
	int target_width, target_height;
	struct avbox_player * const inst = arg;

	/* get the size of the target window */
	avbox_window_getcanvassize(inst->window, &target_width, &target_height);

	/* create an offscreen window for rendering */
	if ((inst->video_window = avbox_window_new(NULL, "video_surface", 0,
		0, 0, inst->state_info.video_res.w, inst->state_info.video_res.h,
		NULL, NULL, NULL)) == NULL) {
		LOG_PRINT_ERROR("Could not create video window!");
	}

	/* clear the off-screen window and set a draw handler
	 * for the target window */
	avbox_window_setbgcolor(inst->video_window, AVBOX_COLOR(0x000000ff));
	avbox_window_clear(inst->video_window);
	return NULL;
}


static void *
avbox_player_destroywindow(void *arg)
{
	struct avbox_player * const inst = arg;
	struct avbox_player_updateargs args;
	ASSERT(inst->window != NULL);
	avbox_window_destroy(inst->video_window);
	inst->video_window = NULL;
	args.inst = inst;
	args.frame = NULL;
	return avbox_player_doupdate(&args);
}

extern int avbox_idle;

/**
 * Video rendering thread.
 */
static void *
avbox_player_video(void *arg)
{
	int delegate_waitable = 0;
	int target_width, target_height, scaled = 0, skip_frame = 0;
	int64_t last_latency = 0;
	struct avbox_player *inst = (struct avbox_player*) arg;
	struct avbox_player_updateargs args = { .inst = inst, .frame = NULL };
	struct avbox_player_packet *packet;
	struct avbox_av_frame *frame;
	struct avbox_delegate *del = NULL;
	struct avbox_object *main_thread_object = avbox_application_object();
	struct timespec tv;
	pthread_cond_t sleep_cond = PTHREAD_COND_INITIALIZER;
	pthread_mutex_t sleep_lock = PTHREAD_MUTEX_INITIALIZER;

	DEBUG_SET_THREAD_NAME("video_output");
	DEBUG_PRINT(LOG_MODULE, "Video renderer started");

	ASSERT(inst != NULL);
	ASSERT(inst->video_window == NULL);

	args.inst = inst;

	/* allocate a recyclable delegate */
	if ((del = avbox_delegate_new(avbox_player_doupdate,
		&args, AVBOX_DELEGATE_RECYCLE)) == NULL) {
		ABORT("Could not allocate recyclable delegate!");
	}

	/* get the size of the target window */
	avbox_window_getcanvassize(inst->window, &target_width, &target_height);

	avbox_checkpoint_enable(&inst->video_output_checkpoint);

	/* signal control thread that we're ready */
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_VIDEOOUT_READY, NULL);

	DEBUG_PRINT("player", "Video renderer ready");

	while (1) {

		avbox_checkpoint_here(&inst->video_output_checkpoint);

		#ifdef DEBUG_LATENCY
		if (avbox_queue_peek(inst->video_frames_q, 0) == NULL) {
			LOG_PRINT_INFO("Waiting for video frames!");
		}
		#endif

		/* get the next packet */
		if (UNLIKELY((packet = avbox_queue_timedpeek(inst->video_frames_q, 250L * 1000LL)) == NULL)) {
			if (errno == EAGAIN) {
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_BUFFER_UNDERRUN, NULL);
				continue;
			} else if (errno == ESHUTDOWN) {
				break;
			} else {
				LOG_VPRINT_ERROR("Error!: avbox_queue_timedpeek() failed: %s",
					strerror(errno));
				goto video_exit;
			}
		}

		/* if this is a control packet handle it */
		switch (packet->type) {
		case AVBOX_PLAYER_PACKET_TYPE_VIDEO:
		{
			frame = packet->video_frame;
			break;
		}
		case AVBOX_PLAYER_PACKET_TYPE_SET_CLOCK:
		{
			const int64_t clock_time = av_rescale_q(packet->clock_value,
				inst->state_info.time_base,
				AV_TIME_BASE_Q);

			DEBUG_VPRINT(LOG_MODULE, "Resetting video clock to %"PRIi64" (was %"PRIi64")",
				packet->clock_value, avbox_stopwatch_time(inst->video_time));

			pthread_mutex_lock(&inst->state_lock);
			const int running = avbox_stopwatch_running(inst->video_time);
			avbox_stopwatch_reset(inst->video_time, clock_time);
			if (running) {
				avbox_stopwatch_start(inst->video_time);
			}
			pthread_mutex_unlock(&inst->state_lock);

			if (avbox_queue_get(inst->video_frames_q) != packet) {
				LOG_PRINT_ERROR("Video packet went missing!!");
				abort();
			}
			release_packet(inst, packet);
			continue;
		}
		default:
			abort();
		}

		if (UNLIKELY(!scaled)) {
			/* create target window from main thread */
			struct avbox_delegate *create_window_del;
			if ((create_window_del = avbox_application_delegate(avbox_player_createwindow, inst)) == NULL) {
				LOG_PRINT_ERROR("Could not create video window");
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
			} else {
				avbox_delegate_wait(create_window_del, NULL);
				if (inst->video_window == NULL) {
					LOG_PRINT_ERROR("Video window not created");
				}
			}

			/* calculate how to scale the video */
			avbox_player_scale2display(inst,
				target_width, target_height, &inst->state_info.scaled_res);
			scaled = 1;
		}

		/* get the frame pts and wait */
		if  (LIKELY(frame->avframe->pts != AV_NOPTS_VALUE)) {
			int64_t current_time;
			const int64_t frame_time = av_rescale_q(frame->avframe->pts,
				inst->state_info.time_base, AV_TIME_BASE_Q);

			#ifdef DEBUG_LATENCY
			/* this takes under 100 uS */
			struct timespec before_gettime, after_gettime;
			clock_gettime(CLOCK_MONOTONIC, &before_gettime);
			current_time = inst->getmastertime(inst);
			clock_gettime(CLOCK_MONOTONIC, &after_gettime);
			#else
			current_time = inst->getmastertime(inst);
			#endif

			if (UNLIKELY(current_time < 0)) {
				LOG_PRINT_ERROR("current_time is negative!");
			}

			/* NOTE: frame_time may be a very large negative interger that
			 * can overflow so we need to check that current_time < frame_time
			 * before substracting.
			 *
			 * This may be the most likely branch but we're optimizing for
			 * the case when we're running late. */
			if (UNLIKELY(current_time < frame_time)) {
				const int64_t delay = frame_time - current_time -
					MIN(last_latency, AVBOX_LATENCY_CORRECT_MAX);
				if (LIKELY(delay > AVBOX_MIN_FRAME_WAIT_US)) {
					tv.tv_sec = 0;
					tv.tv_nsec = delay * 1000LL;

					/* FIXME: The PCM stream is returning the wrong time
					 * sometimes during a seek. That should not be allowed
					 * to happen, it should just block */
					if (UNLIKELY(delay > 250LL * 1000LL)) {
						LOG_VPRINT_WARN("Unreasonable delay of %" PRIi64
							" (frame_time=%" PRIi64 " current_time=%" PRIi64 ")",
							delay, frame_time, current_time);
						tv.tv_nsec = 250LL * 1000LL * 1000LL;
					}

					delay2abstime(&tv);
					pthread_mutex_lock(&sleep_lock);
					pthread_cond_timedwait(&sleep_cond, &sleep_lock, &tv);
					current_time = inst->getmastertime(inst);
					pthread_mutex_unlock(&sleep_lock);

					/* We may wake up too early if playback is paused or
					 * if the audio stream underruns */
					if (UNLIKELY((current_time + (30LL * 1000LL)) < frame_time)) {
						continue;
					}
				}
			}

			#ifdef DEBUG_LATENCY
			if (current_time > (frame_time + (15LL * 1000LL))) {
				LOG_VPRINT_INFO("Woke up too late: (expected=%" PRIi64 " is=%" PRIi64
					" diff=%" PRIi64 " gettime_duration=%" PRIi64 ")",
					frame_time, current_time, current_time - frame_time,
					utimediff(&after_gettime, &before_gettime));
			}
			#endif

			/* if we're running late skip this frame */
			last_latency = MAX(0, current_time - frame_time);
			if (UNLIKELY(last_latency > AVBOX_SKIP_FRAME_THRESHOLD)) {
				if (++skip_frame <= AVBOX_SKIP_FRAME_MAX) {
					av_frame_unref(frame->avframe);
					release_av_frame(inst, frame);
					goto next_frame;
				}
				skip_frame = 0;
			}
		}

		/* if there's an update pending wait for it */
		if (LIKELY(delegate_waitable)) {
			avbox_delegate_wait(del, NULL);
			delegate_waitable = 0;
		}

		#ifdef DEBUG_LATENCY
		const int qcnt = avbox_queue_count(avbox_application_queue());
		if (qcnt > 0 || !avbox_idle) {
			LOG_VPRINT_INFO("Application thread has %i jobs pending and idle=%i",
				qcnt, avbox_idle);
		}
		#endif

		/* send frame to the main thread for presentation */
		args.frame = frame;
		if (UNLIKELY(avbox_object_sendmsg(&main_thread_object,
			AVBOX_MESSAGETYPE_DELEGATE, AVBOX_DISPATCH_UNICAST, del) == NULL)) {
			LOG_VPRINT_ERROR("Could not delegate frame to main thread: %s",
				strerror(errno));
		} else {
			sched_yield();
			delegate_waitable = 1;
		}
next_frame:
		/* update buffer state and signal decoder */
		if (UNLIKELY(avbox_queue_get(inst->video_frames_q) != packet)) {
			LOG_PRINT_ERROR("We peeked one frame but got another one!");
			abort();
		}
		release_packet(inst, packet);
	}

video_exit:
	DEBUG_PRINT("player", "Video renderer exiting");

	avbox_checkpoint_disable(&inst->video_output_checkpoint);

	DEBUG_PRINT(LOG_MODULE, "Freeing last frame");

	/* free any frames left in the queue */
	DEBUG_PRINT(LOG_MODULE, "Freeing decoded frames");
	while ((packet = avbox_queue_get(inst->video_frames_q)) != NULL) {
		if (packet->type == AVBOX_PLAYER_PACKET_TYPE_VIDEO) {
			av_frame_unref(packet->video_frame->avframe);
			release_av_frame(inst, packet->video_frame);
		}
		release_packet(inst, packet);
	}

	/* destroy the video window */
	DEBUG_PRINT(LOG_MODULE, "Destroying video window");
	if (delegate_waitable) {
		avbox_delegate_wait(del, NULL);
		avbox_delegate_destroy(del);
	}
	if (inst->video_window != NULL) {
		if ((del = avbox_application_delegate(avbox_player_destroywindow, inst)) == NULL) {
			LOG_PRINT_ERROR("Could not create video window");
			avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
		} else {
			avbox_delegate_wait(del, NULL);
			ASSERT(inst->video_window == NULL);
		}
	}

	if (inst->last_video_frame != NULL) {
		av_frame_unref(inst->last_video_frame->avframe);
		release_av_frame(inst, inst->last_video_frame);
		inst->last_video_frame = NULL;
	}


	return NULL;
}



static void
avbox_player_destroy_filter_graph(AVFilterGraph *filter_graph,
	AVFilterContext *buffersrc, AVFilterContext *buffersink, AVFrame *frame)
{
	int ret;

	DEBUG_PRINT("player", "Destroying filter graph");

	if (buffersink != NULL) {
		while ((ret = av_buffersink_get_frame(buffersink, frame)) >= 0) {
			DEBUG_PRINT(LOG_MODULE, "There are still frames in the filtergraph!!!");
			av_frame_unref(frame);
		}
		if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
			char err[256];
			av_strerror(ret, err, sizeof(err));
			LOG_VPRINT_ERROR("Could not audio flush filter graph: %s", err);
		}
		avfilter_free(buffersink);
	}
	if (buffersrc != NULL) {
		avfilter_free(buffersrc);
	}
	if (filter_graph != NULL) {
		avfilter_graph_free(&filter_graph);
	}
}


/**
 * Decodes video frames in the background.
 */
static void *
avbox_player_video_decode(void *arg)
{
	int ret, just_flushed = 0, keep_going, time_set = 0, flush_graph = 0;
	struct avbox_player *inst = (struct avbox_player*) arg;
	struct avbox_player_packet *v_packet;
	struct avbox_av_packet *av_packet = NULL;
	char video_filters[512];
	AVCodecContext *dec_ctx;
	AVFrame *video_frame_nat = NULL;

	AVFilterGraph *video_filter_graph = NULL;
	AVFilterContext *video_buffersink_ctx = NULL;
	AVFilterContext *video_buffersrc_ctx = NULL;

	DEBUG_SET_THREAD_NAME("video_decode");
	DEBUG_PRINT("player", "Video decoder starting");

	ASSERT(inst != NULL);
	ASSERT(inst->fmt_ctx != NULL);
	ASSERT(inst->video_stream_index != -1);

	/* open the video codec */
	if ((dec_ctx = avbox_ffmpegutil_opencodeccontext(
		&inst->video_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_VIDEO)) == NULL) {
		LOG_PRINT_ERROR("Could not open video codec context");
		goto decoder_exit;
	}

#ifndef NDEBUG
	char pix_fmt_string[256];
	av_get_pix_fmt_string(pix_fmt_string, sizeof(pix_fmt_string),
		dec_ctx->pix_fmt);
	DEBUG_VPRINT(LOG_MODULE, "Video codec: %s (%s)",
		dec_ctx->codec_descriptor->long_name, dec_ctx->codec_descriptor->name);
	DEBUG_VPRINT(LOG_MODULE, "Pixel format: %s (%s)",
		pix_fmt_string, av_get_pix_fmt_name(dec_ctx->pix_fmt));
	DEBUG_VPRINT(LOG_MODULE, "Resolution: %ix%x",
		dec_ctx->width, dec_ctx->height);
	DEBUG_VPRINT(LOG_MODULE, "Framerate: %d/%d", dec_ctx->framerate,
		dec_ctx->framerate.num, dec_ctx->framerate.den);
#endif

	inst->state_info.video_res.w = dec_ctx->width;
	inst->state_info.video_res.h = dec_ctx->height;
	inst->state_info.time_base.num = inst->fmt_ctx->streams[inst->video_stream_index]->time_base.num;
	inst->state_info.time_base.den = inst->fmt_ctx->streams[inst->video_stream_index]->time_base.den;

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


	for (inst->video_decoder_flushed = 1; 1;) {

		avbox_checkpoint_here(&inst->video_decoder_checkpoint);

		if ((av_packet = avbox_queue_peek(inst->video_packets_q,
			!(inst->flushing & AVBOX_PLAYER_FLUSH_VIDEO))) == NULL) {
			if (errno == EAGAIN) {
				if (inst->video_decoder_flushed ||
					!(inst->flushing & AVBOX_PLAYER_FLUSH_VIDEO)) {
					usleep(50L * 1000L);
					continue;
				}

				/* send the flush packet to the video codec */
				if ((ret = avcodec_send_packet(dec_ctx, NULL)) < 0) {
					LOG_PRINT_ERROR("Error flushing video codec!!!");
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
					goto decoder_exit;
				}

				just_flushed = 1;
				/* fall through */

			} else if (errno == ESHUTDOWN) {
				if (!inst->video_decoder_flushed) {
					if ((ret = avcodec_send_packet(dec_ctx, NULL)) < 0) {
						LOG_PRINT_ERROR("Could not send flush packet to video decoder!");
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
						goto decoder_exit;
					}
					just_flushed = 1;
					/* fall through */
				} else {
					goto decoder_exit;
				}
			} else {
				LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
					strerror(errno));
				break;
			}
		} else {
			/* send packet to codec for decoding */
			if (UNLIKELY((ret  = avcodec_send_packet(dec_ctx, av_packet->avpacket)) < 0)) {
				if (ret == AVERROR(EAGAIN)) {
					/* fall through */
					av_packet = NULL;

				} else if (ret == AVERROR_INVALIDDATA) {
					LOG_PRINT_ERROR("Invalid data sent to video decoder");

				} else {
					char err[256];
					av_strerror(ret, err, sizeof(err));
					LOG_VPRINT_ERROR("Error decoding video packet (%i): %s",
						ret, err);
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
					goto decoder_exit;
				}
			} else {
				inst->video_decoder_flushed = 0;
			}
		}

		/* read decoded frames from codec */
		for (keep_going = 1; keep_going;) {

			/* grab the next frame and add it to the filtergraph */
			if (LIKELY((ret = avcodec_receive_frame(dec_ctx, video_frame_nat)) < 0)) {
				if (ret == AVERROR_EOF) {
					/* send flush packet to filtergraph */
					if (video_filter_graph != NULL) {
						if (av_buffersrc_add_frame(video_buffersrc_ctx, NULL) < 0) {
							LOG_PRINT_ERROR("Could not send flush packet to video buffersrc!");
							avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
							goto decoder_exit;
						}
						flush_graph = 1;
					}

				} else if (ret == AVERROR(EAGAIN)) {
					/* if we're flushing keep trying until EOF */
					if (just_flushed) {
						continue;
					}
				} else {
					LOG_VPRINT_ERROR("ERROR: avcodec_receive_frame() returned %d (video)",
						ret);
				}
				keep_going = 0;

			} else {
				if (video_frame_nat->pkt_dts == AV_NOPTS_VALUE) {
					video_frame_nat->pts = 0;
				} else {
					video_frame_nat->pts = video_frame_nat->pkt_dts;
				}

				if (video_filter_graph == NULL) {
					enum AVPixelFormat pix_fmt;

					/* if we support the pixel format of the decoder then
					 * don't convert it. Otherwise convert to BGRA */
					if ((inst->state_info.pix_fmt =
						avbox_pixfmt_from_libav(dec_ctx->pix_fmt)) == AVBOX_PIXFMT_UNKNOWN) {
						LOG_VPRINT_WARN("Pixel format not supported. Converting %s to BGRA!!",
							av_get_pix_fmt_name(dec_ctx->pix_fmt));
						inst->state_info.pix_fmt = AVBOX_PIXFMT_BGRA;
						pix_fmt = AV_PIX_FMT_BGRA;
					} else {
						pix_fmt = dec_ctx->pix_fmt;
					}

					/* initialize video filter graph */
					strcpy(video_filters, "null");
					DEBUG_VPRINT("player", "Video width: %i height: %i",
						dec_ctx->width, dec_ctx->height);
					DEBUG_VPRINT("player", "Video filters: %s", video_filters);
					if (avbox_ffmpegutil_initvideofilters(inst->fmt_ctx, dec_ctx,
						&video_buffersink_ctx, &video_buffersrc_ctx, &video_filter_graph,
						pix_fmt, video_filters, inst->video_stream_index) < 0) {
						LOG_PRINT_ERROR("Could not initialize filtergraph!");
						goto decoder_exit;
					}
				}

				/* push the decoded frame into the filtergraph */
				if (UNLIKELY((ret = av_buffersrc_add_frame_flags(video_buffersrc_ctx,
					video_frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF |
					AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT)) < 0)) {
					char err[256];
					av_strerror(ret, err, sizeof(err));
					LOG_VPRINT_ERROR("Error feeding video filtergraph (%i): %s",
						ret, err);
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
					goto decoder_exit;
				}
			}

			/* pull filtered frames from the filtergraph */
			while (video_filter_graph != NULL) {
				struct avbox_av_frame *video_frame_flt;
				if ((video_frame_flt = acquire_av_frame(inst)) == NULL) {
					LOG_PRINT_ERROR("Cannot allocate AVFrame: Out of memory!");
					continue;
				}

				/* get the next frame */
				if ((ret = av_buffersink_get_frame(video_buffersink_ctx, video_frame_flt->avframe)) < 0) {
					if (ret == AVERROR_EOF) {
						DEBUG_PRINT(LOG_MODULE, "Video filtergraph reached EOF");
						release_av_frame(inst, video_frame_flt);
						flush_graph = 0;
						break;

					} else if (UNLIKELY(ret == AVERROR(EAGAIN))) {
						release_av_frame(inst, video_frame_flt);

						/* if we're flushing keep trying until EOF */
						if (flush_graph) {
							continue;
						} else {
							break;
						}
					} else {
						LOG_VPRINT_ERROR("Could not get video frame from filtergraph (ret=%i)",
							ret);
						release_av_frame(inst, video_frame_flt);
						goto decoder_exit;
					}
				}

				ASSERT(video_buffersink_ctx->inputs[0]->time_base.num == inst->fmt_ctx->streams[inst->video_stream_index]->time_base.num);
				ASSERT(video_buffersink_ctx->inputs[0]->time_base.den == inst->fmt_ctx->streams[inst->video_stream_index]->time_base.den);

				video_frame_flt->avframe->pts = av_frame_get_best_effort_timestamp(video_frame_flt->avframe);

				if (!time_set) {
					/* NOTE: We're getting the time_base from the AVStream object because
					 * the ones on AVCodecContext and AVFrame are wrong */
					const int64_t pts = av_rescale_q(video_frame_flt->avframe->pts,
						inst->fmt_ctx->streams[inst->video_stream_index]->time_base, AV_TIME_BASE_Q);

					if ((v_packet = acquire_packet(inst)) == NULL) {
						LOG_VPRINT_ERROR("Could not allocate clock packet: %s",
							strerror(errno));
						release_av_frame(inst, video_frame_flt);
						video_frame_flt = NULL;
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
						goto decoder_exit;
					}

					v_packet->type = AVBOX_PLAYER_PACKET_TYPE_SET_CLOCK;
					v_packet->clock_value = pts;
					time_set = 1;

					while (avbox_queue_put(inst->video_frames_q, v_packet) == -1) {
						if (errno == EAGAIN) {
							continue;
						} else if (errno == ESHUTDOWN) {
							LOG_PRINT_ERROR("Video frames queue closed unexpectedly!");
						} else {
							LOG_VPRINT_ERROR("Error: avbox_queue_put() failed: %s",
								strerror(errno));
						}
						release_av_frame(inst, video_frame_flt);
						release_packet(inst, v_packet);
						video_frame_flt = NULL;
						goto decoder_exit;
					}
				}

				/* allocate packet */
				if ((v_packet = acquire_packet(inst)) == NULL) {
					LOG_VPRINT_ERROR("Could not allocate clock packet: %s",
						strerror(errno));
					release_av_frame(inst, video_frame_flt);
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
					goto decoder_exit;
				}

				v_packet->type = AVBOX_PLAYER_PACKET_TYPE_VIDEO;
				v_packet->video_frame = video_frame_flt;

				/* add frame to decoded frames queue */
				while (avbox_queue_put(inst->video_frames_q, v_packet) == -1) {
					if (errno == EAGAIN) {
						continue;
					} else if (errno == ESHUTDOWN) {
						LOG_PRINT_ERROR("Video frames queue closed unexpectedly!");
					} else {
						LOG_VPRINT_ERROR("Error: avbox_queue_put() failed: %s",
							strerror(errno));
					}
					release_av_frame(inst, video_frame_flt);
					release_packet(inst, v_packet);
					goto decoder_exit;
				}
			}
			av_frame_unref(video_frame_nat);
		}

		/* dequeue and free video packet */
		if (av_packet != NULL) {
			if (avbox_queue_get(inst->video_packets_q) != av_packet) {
				LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result: %s",
					strerror(errno));
				goto decoder_exit;
			}
			av_packet_unref(av_packet->avpacket);
			release_av_packet(inst, av_packet);
			av_packet = NULL;
		}

		/* if we just flushed the codec pipeline flush the
		 * buffers so we can keep using it */
		if (just_flushed) {
			DEBUG_PRINT(LOG_MODULE, "Video decoder flushed");
			avcodec_flush_buffers(dec_ctx);
			if (video_filter_graph != NULL) {
				avbox_player_destroy_filter_graph(video_filter_graph,
					video_buffersrc_ctx, video_buffersink_ctx, video_frame_nat);
			}
			video_filter_graph = NULL;
			inst->video_decoder_flushed = 1;
			just_flushed = 0;
			time_set = 0;
		}
	}
decoder_exit:
	DEBUG_PRINT("player", "Video decoder exiting");

	avbox_checkpoint_disable(&inst->video_decoder_checkpoint);

	/* signal the video thread to exit and join it */
	if (inst->video_frames_q != NULL) {
		avbox_queue_close(inst->video_frames_q);
	}

	if (video_filter_graph != NULL) {
		ASSERT(video_frame_nat != NULL);
		avbox_player_destroy_filter_graph(video_filter_graph,
			video_buffersrc_ctx, video_buffersink_ctx, video_frame_nat);
	}

	if (dec_ctx != NULL) {
		DEBUG_PRINT(LOG_MODULE, "Flushing video decoder");
		while (avcodec_receive_frame(dec_ctx, video_frame_nat) == 0) {
			DEBUG_PRINT(LOG_MODULE, "There are still frames on video decoder!!!");
			av_frame_unref(video_frame_nat);
		}
		avcodec_flush_buffers(dec_ctx);
		avcodec_close(dec_ctx);
		avcodec_free_context(&dec_ctx);
	}
	if (video_frame_nat != NULL) {
		av_frame_free(&video_frame_nat);
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
	int ret, keep_going, just_flushed = 0, time_set = 0, flush_graph = 0;
	int stream_index = -1;
	struct avbox_syncarg * const syncarg = arg;
	struct avbox_player * const inst = avbox_syncarg_data(syncarg);
	struct avbox_av_packet * av_packet = NULL;
	const char *audio_filters ="aresample=48000,aformat=sample_fmts=s16:channel_layouts=stereo";
	AVCodecContext *dec_ctx = NULL;
	AVFrame *audio_frame_nat = NULL;
	AVFilterGraph *filter_graph = NULL;
	AVFilterContext *audio_buffersink_ctx = NULL;
	AVFilterContext *audio_buffersrc_ctx;
	const char *sample_fmt_name;

	DEBUG_SET_THREAD_NAME("audio_decoder");

	ASSERT(inst != NULL);
	ASSERT(inst->fmt_ctx != NULL);
	ASSERT(inst->audio_packets_q != NULL);
	ASSERT(inst->audio_stream_index != -1);

	DEBUG_PRINT("player", "Audio decoder starting");

	/* allocate audio frames */
	audio_frame_nat = av_frame_alloc();
	if (audio_frame_nat == NULL) {
		LOG_PRINT_ERROR("Could not allocate audio frames");
		avbox_syncarg_return(syncarg, NULL);
		goto end;
	}

	/* initialize audio filter graph */
	DEBUG_VPRINT("player", "Audio filters: %s", audio_filters);

	avbox_checkpoint_enable(&inst->audio_decoder_checkpoint);
	avbox_syncarg_return(syncarg, NULL);

	/* signl control thread that we're ready */
	if (inst->play_state == AVBOX_PLAYER_PLAYSTATE_AUDIODEC) {
		avbox_player_sendctl(inst, AVBOX_PLAYERCTL_AUDIODEC_READY, NULL);
	}

	DEBUG_PRINT("player", "Audio decoder ready");


	for (inst->audio_decoder_flushed = 1;;) {

		avbox_checkpoint_here(&inst->audio_decoder_checkpoint);

		/* wait for the stream decoder to give us some packets */
		if ((av_packet = avbox_queue_peek(inst->audio_packets_q,
			!(inst->flushing & AVBOX_PLAYER_FLUSH_AUDIO))) == NULL) {
			if (errno == EAGAIN) {
				if (inst->audio_decoder_flushed ||
					!(inst->flushing & AVBOX_PLAYER_FLUSH_AUDIO)) {
					usleep(50L * 1000L);
					continue;
				} else {
					ret = avcodec_send_packet(dec_ctx, NULL);
					just_flushed = 1;
					/* fall through */
				}
			} else if (errno == ESHUTDOWN) {
				if (dec_ctx == NULL) {
					goto end;
				} else if (inst->audio_decoder_flushed) {
					break;
				} else {
					if ((ret = avcodec_send_packet(dec_ctx, NULL)) < 0) {
						LOG_PRINT_ERROR("Error sending flush packet to audio decoder!");
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
						goto end;
					}
					just_flushed = 1;
					/* fall through */
				}
			} else {
				LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
					strerror(errno));
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
				goto end;
			}
		} else {
			if (dec_ctx == NULL || av_packet->avpacket->stream_index != stream_index) {
				if (dec_ctx != NULL) {
					if (inst->audio_decoder_flushed) {
						DEBUG_PRINT(LOG_MODULE, "Closing audio decoder");
						avcodec_close(dec_ctx);
						avcodec_free_context(&dec_ctx);
					} else {
						if ((ret = avcodec_send_packet(dec_ctx, NULL)) < 0) {
							LOG_PRINT_ERROR("Error sending flush packet to audio decoder!");
							avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
							goto end;
						}
						just_flushed = 1;
						av_packet = NULL;
					}
				}
				if (av_packet != NULL) {
					DEBUG_VPRINT(LOG_MODULE, "Opening audio decoder for stream %i",
						av_packet->avpacket->stream_index);
					if ((dec_ctx = avbox_ffmpegutil_opencodeccontext(
						&inst->audio_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_AUDIO)) == NULL) {
						LOG_PRINT_ERROR("Could not open audio codec!");
						goto end;
					}
					stream_index = av_packet->avpacket->stream_index;
					time_set = 0;
				}
			}

			if (av_packet != NULL) {
				/* send packets to codec for decoding */
				if ((ret = avcodec_send_packet(dec_ctx, av_packet->avpacket)) < 0) {
					if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
						av_packet = NULL;
						/* fall through */

					} else if (ret == AVERROR(EINVAL) || ret == AVERROR_INVALIDDATA) {
						char err[256];
						av_strerror(ret, err, sizeof(err));
						LOG_VPRINT_ERROR("Invalid data sent to audio decoder: %s",
							err);
						ret = 0; /* so we still dequeue it */

					} else if (ret == AVERROR(ENOMEM)) {
						LOG_PRINT_ERROR("Audio decoder out of memory");
						abort();

					} else {
						char err[256];
						av_strerror(ret, err, sizeof(err));
						LOG_VPRINT_ERROR("Error decoding audio(%i): %s",
							ret, err);
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
						goto end;
					}
				}
				inst->audio_decoder_flushed = 0;
			}
		}

		for (keep_going = 1; keep_going;) {
			/* get the next frame from the decoder */
			if ((ret = avcodec_receive_frame(dec_ctx, audio_frame_nat)) != 0) {
				if (ret == AVERROR_EOF) {
					if (filter_graph != NULL) {
						/* tell the filtergraph to flush */
						if ((ret = av_buffersrc_add_frame(audio_buffersrc_ctx, NULL)) < 0) {
							char err[256];
							av_strerror(ret, err, sizeof(err));
							LOG_VPRINT_ERROR("Error while feeding the audio filtergraph: %s (channels=%i|layout=0x%"PRIx64")",
								err, audio_frame_nat->channels, audio_frame_nat->channel_layout);
							avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
							goto end;
						}
						flush_graph = 1;
					}
					/* fall through */
				}
				else if (ret == AVERROR(EAGAIN)) {
					/* if we're flushing keep trying until EOF */
					if (just_flushed) {
						continue;
					}
					/* fall through */
				}
				else {
					LOG_VPRINT_ERROR("ERROR!: avcodec_receive_frame() returned %d (audio)",
						AVERROR(ret));
					/* if we're flushing keep trying until EOF */
					if (just_flushed) {
						continue;
					}
				}
				keep_going = 0;
			} else {
				if (!dec_ctx->channel_layout) {
					dec_ctx->channel_layout = av_get_default_channel_layout(
						dec_ctx->channels);
				}

				/* initialize the filtergraph is needed */
				if (filter_graph == NULL) {
					if ((sample_fmt_name = av_get_sample_fmt_name(dec_ctx->sample_fmt)) == NULL) {
						sample_fmt_name = "fltp";
					}

					DEBUG_PRINT(LOG_MODULE, "Initializing filtergraph");

					/* initialize audio filtergraph */
					if (avbox_ffmpegutil_initaudiofilters(
						&audio_buffersink_ctx, &audio_buffersrc_ctx,
						&filter_graph, 	audio_filters, dec_ctx->sample_rate,
						dec_ctx->time_base, dec_ctx->channel_layout, sample_fmt_name) < 0) {
						LOG_PRINT_ERROR("Could not init filter graph!");
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
						av_frame_unref(audio_frame_nat);
						goto end;
					}
				}

				/* push the audio data from decoded frame into the filtergraph */
				if (UNLIKELY((ret = av_buffersrc_add_frame_flags(
					audio_buffersrc_ctx, audio_frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0)) {
					char err[256];
					av_strerror(ret, err, sizeof(err));
					LOG_VPRINT_ERROR("Error while feeding the audio filtergraph: %s (channels=%i|layout=0x%"PRIx64")",
						err, audio_frame_nat->channels, audio_frame_nat->channel_layout);
					av_frame_unref(audio_frame_nat);
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
					keep_going = 0;
				}
			}

			sched_yield();

			/* pull filtered audio from the filtergraph */
			while (filter_graph != NULL) {
				struct avbox_av_frame * const frame = acquire_av_frame(inst);
				if (frame == NULL) {
					LOG_PRINT_ERROR("Could not allocate avframe!");
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
					goto end;
				}
				if ((ret = av_buffersink_get_frame(audio_buffersink_ctx, frame->avframe)) < 0) {
					if (UNLIKELY(ret == AVERROR(EAGAIN))) {

						release_av_frame(inst, frame);

						/* if we're flushing keep trying until EOF */
						if (flush_graph) {
							continue;
						}
						break;
					} else if (ret == AVERROR_EOF) {
						DEBUG_PRINT(LOG_MODULE, "Audio filtergraph reached EOF");
						flush_graph = 0;
						release_av_frame(inst, frame);
						break;
					} else {
						LOG_PRINT_ERROR("Error reading from buffersink");
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
						release_av_frame(inst, frame);
						goto end;
					}
				}

				/* if this is the first frame after a flush then set the audio stream
				 * clock to it's pts */
				if (UNLIKELY(!time_set)) {
					int64_t pts;
					pts = av_frame_get_best_effort_timestamp(frame->avframe);
					pts = av_rescale_q(pts,
						inst->fmt_ctx->streams[inst->audio_stream_index]->time_base,
						AV_TIME_BASE_Q);
					avbox_audiostream_setclock(inst->audio_stream, pts);
					inst->getmastertime = avbox_player_getaudiotime;
					time_set = 1;
				}

				/* write frame to audio stream and free it */
				while (avbox_audiostream_write(inst->audio_stream,
					frame->avframe->data[0], frame->avframe->nb_samples, frame) == -1) {
					if (errno == EAGAIN) {
						continue;
					} else {
						LOG_VPRINT_ERROR("Could not write audio frames: %s",
							strerror(errno));
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
						release_av_frame(inst, frame);
						break;
					}
				}
				sched_yield();
			}
		}

		/* remove packet from queue */
		if (av_packet != NULL) {
			if (avbox_queue_get(inst->audio_packets_q) != av_packet) {
				LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result (%p): %s",
					av_packet, strerror(errno));
				avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
				goto end;
			}
			/* free packet */
			av_packet_unref(av_packet->avpacket);
			release_av_packet(inst, av_packet);
			av_packet = NULL;
		}

		if (just_flushed) {
			struct avbox_av_frame * const frame = acquire_av_frame(inst);
			DEBUG_PRINT(LOG_MODULE, "Audio decoder flushed");
			avcodec_flush_buffers(dec_ctx);
			if (frame == NULL) {
				LOG_PRINT_ERROR("Could not allocate frame to flush filter graph");
			} else {
				avbox_player_destroy_filter_graph(filter_graph,
					audio_buffersrc_ctx, audio_buffersink_ctx, frame->avframe);
				release_av_frame(inst, frame);
			}
			inst->audio_decoder_flushed = 1;
			filter_graph = NULL;
			just_flushed = 0;
			time_set = 0;
		}
	}
end:
	DEBUG_PRINT("player", "Audio decoder exiting");

	avbox_checkpoint_disable(&inst->audio_decoder_checkpoint);

	if (filter_graph != NULL) {
		struct avbox_av_frame * const frame = acquire_av_frame(inst);
		if (frame == NULL) {
			LOG_PRINT_ERROR("Could not allocate frame to flush filter graph!");
		} else {
			avbox_player_destroy_filter_graph(filter_graph,
				audio_buffersrc_ctx, audio_buffersink_ctx, frame->avframe);
			release_av_frame(inst, frame);
		}
	}

	if (audio_frame_nat != NULL) {
		av_frame_free(&audio_frame_nat);
	}
	if (dec_ctx != NULL) {
		DEBUG_VPRINT(LOG_MODULE, "Flushing audio decoder (audio_decoder_flushed=%d)",
			inst->audio_decoder_flushed);
		avcodec_flush_buffers(dec_ctx);
		avcodec_close(dec_ctx);
		avcodec_free_context(&dec_ctx);
	}

	DEBUG_PRINT("player", "Audio decoder bailing out");

	return NULL;
}


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
avbox_player_audiostream_callback(struct avbox_audiostream * stream,
	int msg, void * const data, void * const context)
{
	struct avbox_player * const inst = context;
	ASSERT(inst != NULL);

	switch (msg) {
	case AVBOX_AUDIOSTREAM_UNDERRUN:
	{
		ASSERT(inst->audio_stream == stream);
		avbox_player_sendctl(inst, AVBOX_PLAYERCTL_AUDIO_STREAM_UNDERRUN, NULL);
		break;
	}
	case AVBOX_AUDIOSTREAM_CRITICAL_ERROR:
	{
		avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
		break;
	}
	case AVBOX_AUDIOSTREAM_PACKET_RELEASED:
	{
		struct avbox_av_frame * const frame = data;
		av_frame_unref(frame->avframe);
		release_av_frame(inst, frame);
		break;
	}
	default:
		ABORT("Unexepected audio message");
	}
}


/**
 * This is the main decoding loop. It reads the stream and feeds
 * encoded frames to the decoder threads.
 */
static void*
avbox_player_stream_parse(void *arg)
{
	int res;
	AVDictionary *stream_opts = NULL;
	struct avbox_player *inst = (struct avbox_player*) arg;
	int prefered_video_stream = -1;

	DEBUG_SET_THREAD_NAME("stream_input");

	ASSERT(inst != NULL);
	ASSERT(inst->media_file != NULL);
	ASSERT(inst->window != NULL);
	ASSERT(inst->status == MB_PLAYER_STATUS_PLAYING || inst->status == MB_PLAYER_STATUS_BUFFERING);
	ASSERT(inst->fmt_ctx == NULL);
	ASSERT(inst->audio_stream == NULL);
	ASSERT(inst->video_packets_q == NULL);
	ASSERT(inst->video_frames_q == NULL);
	ASSERT(inst->audio_packets_q == NULL);
	ASSERT(inst->audio_stream_index == -1);
	ASSERT(inst->video_stream_index == -1);
	ASSERT(inst->still_frame == 0);
	ASSERT(inst->last_video_frame == NULL);

	inst->audio_decoder_flushed = 1;
	inst->video_decoder_flushed = 1;
	inst->underrun = 0;
	inst->getmastertime = avbox_player_getsystemtime;
	inst->paused = 0;

	DEBUG_VPRINT("player", "Attempting to play '%s'", inst->media_file);

	avbox_player_settitle(inst, inst->media_file);

	/* allocate format context */
	if ((inst->fmt_ctx = avformat_alloc_context()) == NULL) {
		LOG_PRINT_ERROR("Could not allocate format context!");
		goto decoder_exit;
	}

	/* set the AVIO context */
	if (inst->stream.self != NULL) {
		ASSERT(inst->stream.avio != NULL);
		inst->fmt_ctx->pb = inst->stream.avio;
		inst->fmt_ctx->ctx_flags |= AVFMTCTX_NOHEADER;
	}

	/* enter underrun state
	 * NOTE: We need to set the underrun flag first because underrun
	 * is entered when the stream buffers reach zero, but by the time
	 * this message is handled there may already be some data in the
	 * queue in which case it will be ignored */
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_BUFFER_UNDERRUN, NULL);

	/* open file */
	av_dict_set(&stream_opts, "timeout", "30000000", 0);
	if ((res = avformat_open_input(&inst->fmt_ctx, inst->media_file, NULL, &stream_opts)) != 0) {
		char err[256];
		av_strerror(res, err, sizeof(err));
		LOG_VPRINT_ERROR("Could not open stream '%s': %s",
			inst->media_file, err);
		goto decoder_exit;
	}

	if (avformat_find_stream_info(inst->fmt_ctx, NULL) < 0) {
		LOG_PRINT_ERROR("Could not find stream info!");
		goto decoder_exit;
	}

	/* if the stream doesn't set the title we need to */
	if (inst->stream.self == NULL)
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
	}

	if (inst->stream.self == NULL || !inst->stream.manages_position) {
		inst->state_info.duration = inst->fmt_ctx->duration;
	}

	/* create audio stream */
	if ((inst->audio_stream = avbox_audiostream_new(
		AVBOX_BUFFER_AUDIO,
		avbox_player_audiostream_callback, inst)) == NULL) {
		goto decoder_exit;
	}

	if ((inst->audio_packets_q = avbox_queue_new(MB_AUDIO_BUFFER_PACKETS)) == NULL) {
		LOG_VPRINT_ERROR("Could not create audio packets queue: %s!",
			strerror(errno));
		goto decoder_exit;
	}

	avbox_queue_setname(inst->audio_packets_q, "audio_packets");

	/* if there's an audio stream start the audio decoder */
	if ((inst->audio_stream_index =
		av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, prefered_video_stream, NULL, 0)) >= 0) {
		DEBUG_VPRINT("player", "Audio stream found %i", inst->audio_stream_index);
		inst->getmastertime = avbox_player_getaudiotime; /* video is slave to audio */

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

		avbox_queue_setname(inst->video_frames_q, "video_frames");
		avbox_queue_setname(inst->video_packets_q, "video_packets");

		/* if the audio is running the show then set the
		 * video queue to unlimited size */
		if (inst->audio_stream_index != -1) {
			avbox_queue_setsize(inst->video_frames_q, AVBOX_BUFFER_VIDEO);
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
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STREAM_READY, NULL);

	DEBUG_PRINT("player", "Stream decoder ready");

	/* start decoding */
	while (LIKELY(!inst->stream_quit)) {

		avbox_checkpoint_here(&inst->stream_parser_checkpoint);

		struct avbox_av_packet * const av_packet = acquire_av_packet(inst);
		if (av_packet == NULL) {
			ABORT("Out of memory");
		}

		/* read the next input packet */
		if (UNLIKELY((res = av_read_frame(inst->fmt_ctx, av_packet->avpacket)) < 0)) {
			if (res == AVERROR_EOF) {
				goto decoder_exit;
			} else {
				char buf[256];
				av_strerror(res, buf, sizeof(buf));
				LOG_VPRINT_ERROR("Could not read frame: %s", buf);
				goto decoder_exit;
			}
		}

		if (inst->stream.self == NULL || !inst->stream.manages_position) {
			inst->state_info.pos = av_rescale_q(av_packet->avpacket->pts,
				inst->fmt_ctx->streams[av_packet->avpacket->stream_index]->time_base,
				AV_TIME_BASE_Q);
		}

		if (av_packet->avpacket->stream_index == inst->video_stream_index) {
			while (avbox_queue_put(inst->video_packets_q, av_packet) == -1) {
				if (errno == EAGAIN) {
					continue;
				} else if (errno == ESHUTDOWN) {
					LOG_PRINT_ERROR("Video packets queue shutdown! Aborting parser!");
					av_packet_unref(av_packet->avpacket);
					release_av_packet(inst, av_packet);
					goto decoder_exit;
				}
				LOG_VPRINT_ERROR("Could not add packet to queue: %s",
					strerror(errno));
				av_packet_unref(av_packet->avpacket);
				release_av_packet(inst, av_packet);
				goto decoder_exit;
			}

		} else if (inst->fmt_ctx->streams[av_packet->avpacket->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {

			/* if we're waiting for the audio stream... */
			if (inst->audio_stream_index == -2) {
				if (inst->audio_stream_id != -1) {
					AVStream * const stream = inst->fmt_ctx->streams[av_packet->avpacket->stream_index];
					DEBUG_VPRINT(LOG_MODULE, "Waiting for stream %i, checking: id=%x, index=%d",
						inst->audio_stream_id, stream->id, av_packet->avpacket->stream_index);
					if (stream->id  == inst->audio_stream_id) {
						DEBUG_VPRINT(LOG_MODULE, "Selecting stream: %i",
							av_packet->avpacket->stream_index);
						inst->audio_stream_index = av_packet->avpacket->stream_index;
					}
				} else {
					inst->audio_stream_index = -1;
				}
			}

			if (av_packet->avpacket->stream_index == inst->audio_stream_index) {
				while (avbox_queue_put(inst->audio_packets_q, av_packet) == -1) {
					if (errno == EAGAIN) {
						continue;
					} else if (errno == ESHUTDOWN) {
						LOG_PRINT_ERROR("Audio packets queue shutdown! Aborting parser!");
						av_packet_unref(av_packet->avpacket);
						release_av_packet(inst, av_packet);
						goto decoder_exit;
					}
					LOG_VPRINT_ERROR("Could not enqueue packet: %s",
						strerror(errno));
					av_packet_unref(av_packet->avpacket);
					release_av_packet(inst, av_packet);
					goto decoder_exit;
				}
			} else {
				av_packet_unref(av_packet->avpacket);
				release_av_packet(inst, av_packet);
			}
		} else {
			av_packet_unref(av_packet->avpacket);
			release_av_packet(inst, av_packet);
		}
		sched_yield();
	}

decoder_exit:
	DEBUG_VPRINT("player", "Stream parser exiting (quit=%i)",
		inst->stream_quit);

	/* disable the checkpoint */
	avbox_checkpoint_disable(&inst->stream_parser_checkpoint);

	pthread_mutex_lock(&inst->state_lock);
	inst->stream_exiting  = 1;
	pthread_mutex_unlock(&inst->state_lock);

	if (inst->video_stream_index != -1) {
		ASSERT(inst->video_packets_q != NULL);
		avbox_queue_close(inst->video_packets_q);
	}

	if (inst->audio_packets_q != NULL) {
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

	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STREAM_EXIT, NULL);

	DEBUG_PRINT("player", "Stream parser thread bailing out");

	return NULL;
}


static inline int
avbox_player_stream_checkpoint_wait(struct avbox_player * const inst, int64_t timeout)
{
	if (inst->stream.self == NULL) {
		return avbox_checkpoint_wait(&inst->stream_parser_checkpoint, timeout);
	} else {
		return inst->stream.is_blocking(inst->stream.self) ||
			avbox_checkpoint_wait(&inst->stream_parser_checkpoint, timeout);
	}
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
		if (inst->audio_packets_q != NULL) {
			avbox_queue_wake(inst->audio_packets_q);
		}
		if (inst->video_packets_q != NULL) {
			avbox_queue_wake(inst->video_packets_q);
		}
	} while (!avbox_player_stream_checkpoint_wait(inst, 50L * 1000L));

	if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_VIDEODEC && inst->video_stream_index != -1) {
		DEBUG_PRINT(LOG_MODULE, "Halting video decoder...");
		avbox_checkpoint_halt(&inst->video_decoder_checkpoint);
		do {
			avbox_queue_wake(inst->video_packets_q);
			avbox_queue_wake(inst->video_frames_q);
		} while (!avbox_checkpoint_wait(&inst->video_decoder_checkpoint, 50L * 1000L));

		DEBUG_PRINT(LOG_MODULE, "Halting video output...");
		avbox_checkpoint_halt(&inst->video_output_checkpoint);
		do {
			avbox_queue_wake(inst->video_frames_q);
		} while (!avbox_checkpoint_wait(&inst->video_output_checkpoint, 50L * 1000L));
	}

	if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_AUDIODEC) {
		DEBUG_VPRINT(LOG_MODULE, "Halting audio. Play_state=%i",
			inst->play_state);
		avbox_checkpoint_halt(&inst->audio_decoder_checkpoint);
		do {
			if (inst->audio_packets_q != NULL) {
				avbox_queue_wake(inst->audio_packets_q);
			}
		} while (!avbox_checkpoint_wait(&inst->audio_decoder_checkpoint, 50L * 1000L));
		avbox_audiostream_pause(inst->audio_stream);
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

	if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_AUDIODEC) {
		avbox_checkpoint_continue(&inst->audio_decoder_checkpoint);
		avbox_audiostream_resume(inst->audio_stream);
	}

	if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_VIDEODEC && inst->video_stream_index != -1) {
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
	if (!inst->paused++) {
		/* NOTE: The reason for this is because the video output thread wakes
		 * every few miliseconds even when paused. So this saves a little power
		 * while paused and perhaps it can speed up buffering a tiny bit since
		 * it frees some cpu while not playing */
		if (inst->video_stream_index != -1 && inst->audio_stream_index == -1) {
			avbox_checkpoint_halt(&inst->video_output_checkpoint);
			do {
				avbox_queue_wake(inst->video_frames_q);
			} while (!avbox_checkpoint_wait(&inst->video_output_checkpoint, 50L * 1000L));
		}

		pthread_mutex_lock(&inst->state_lock);
		avbox_stopwatch_stop(inst->video_time);
		pthread_mutex_unlock(&inst->state_lock);

		avbox_audiostream_pause(inst->audio_stream);
	}
}


/**
 * Resume the running stream.
 */
static void
avbox_player_doresume(struct avbox_player * const inst)
{
	if (!inst->paused) {
		return;
	}

	if (!--inst->paused) {
		avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);

		if (inst->video_stream_index != -1 && inst->audio_stream_index == -1) {
			avbox_checkpoint_continue(&inst->video_output_checkpoint);
		}

		pthread_mutex_lock(&inst->state_lock);
		avbox_stopwatch_start(inst->video_time);
		pthread_mutex_unlock(&inst->state_lock);

		if (avbox_audiostream_ispaused(inst->audio_stream)) {
			if (inst->audio_stream_index != -1 && inst->video_stream_index != -1) {
				while (avbox_queue_count(inst->video_frames_q) == 0 &&
					!avbox_audiostream_blocking(inst->audio_stream) && !inst->stopping) {
					usleep(10L * 1000LL);
				}
			}
			avbox_audiostream_resume(inst->audio_stream);
		}
	}
}


/**
 * Stop playing stream.
 */
static void
avbox_player_dostop(struct avbox_player * const inst)
{
	if (inst->stream.self != NULL) {
		inst->stream.close(inst->stream.self);
		return;
	}

	/* tell the stream thread to quit */
	inst->stream_quit = 1;

	/* if the video is paused then unpause it first. */
	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		DEBUG_PRINT("player", "Unpausing stream");
		avbox_player_doresume(inst);
	}
}

struct avbox_player_openstream_args
{
	struct avbox_player * inst;
	const char *path;
};


static void*
avbox_player_openstream(void *arg)
{
	struct avbox_player_openstream_args * const osa = arg;

	(void) osa;

	if (0) {
		/* nothing */
	}

#ifdef ENABLE_DVD
	/* if this is a DVD open it */
	else if (!strncmp("dvd:", osa->path, 4)) {
		if (avbox_dvdio_open(osa->path + 4, osa->inst, &osa->inst->stream) == NULL) {
			LOG_VPRINT_ERROR("Could not open DVD: %s",
				strerror(errno));
			ASSERT(osa->inst->stream.self == NULL);
			return (void*) -1;
		}
	}
#endif

	else if (!strncmp("magnet:", osa->path, 7) ||
		(!strncmp("http", osa->path, 4))) {
		if (avbox_torrentin_open(osa->path, osa->inst, &osa->inst->stream) == NULL) {
			LOG_VPRINT_ERROR("Could not open torrent stream: %s",
				strerror(errno));
			ASSERT(osa->inst->stream.self == NULL);
			return (void*) -1;
		}
	}

	return (void*) 0;
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

	/* update the player status */
	inst->stream_percent = 0;
	inst->state_info.pos = 0;
	inst->state_info.duration = 0;
	inst->play_state = AVBOX_PLAYER_PLAYSTATE_STREAM;
	avbox_player_settitle(inst, path);
	avbox_player_updatestatus(inst, MB_PLAYER_STATUS_BUFFERING);

	/* multicast and anycast messages are broken and are always
	 * sent to the thread of the 1st recipient. So until this is
	 * fixed we open the stream from the main thread so it can
	 * request input */
	struct avbox_player_openstream_args open_stream;
	struct avbox_delegate *del;
	open_stream.inst = inst;
	open_stream.path = path;
	if ((del = avbox_application_delegate(avbox_player_openstream, &open_stream)) == NULL) {
		avbox_player_throwexception(inst, "Could not delegate streamopen");
		return;
	} else {
		void *ret;
		avbox_delegate_wait(del, &ret);
		if (ret == (void*) -1) {
			DEBUG_PRINT(LOG_MODULE, "Could not open stream. Returning to READY state");
			avbox_player_updatestatus(inst, MB_PLAYER_STATUS_READY);
			inst->play_state = AVBOX_PLAYER_PLAYSTATE_READY;
			return;
		}
	}

	/* initialize player object */
	const char *old_media_file = inst->media_file;
	inst->media_file = strdup(path);
	if (old_media_file != NULL) {
		free((void*) old_media_file);
	}

	/* start the main decoder worker */
	inst->stream_quit = 0;
	ASSERT(inst->stream_input_worker == NULL);
	ASSERT(inst->stream_input_thread != NULL);
	if ((inst->stream_input_worker = avbox_thread_delegate(
		inst->stream_input_thread, avbox_player_stream_parse, inst)) == NULL) {
		LOG_VPRINT_ERROR("Could not start input thread: %s",
			strerror(errno));
		inst->play_state = AVBOX_PLAYER_PLAYSTATE_READY;
		avbox_player_updatestatus(inst, MB_PLAYER_STATUS_READY);
	}
}


static void
avbox_player_clear_still_frame(struct avbox_player * const inst)
{
	/* clear still frame condition */
	if (inst->still_frame) {
		DEBUG_PRINT(LOG_MODULE, "Clering still frame");
		if (inst->still_frame_timer_id != -1) {
			avbox_timer_cancel(inst->still_frame_timer_id);
			avbox_syncarg_return(inst->still_frame_waiter, NULL);
			inst->still_frame_timer_id = -1;
		}
		inst->still_frame = 0;
	}
}


static void
avbox_player_drop(struct avbox_player * const inst)
{
	struct avbox_player_packet *v_packet;
	struct avbox_av_packet *av_packet;

	/* set the flushing flag and make sure the
	 * decoders are aware */
	inst->flushing = AVBOX_PLAYER_FLUSH_ALL;
	if (inst->video_packets_q != NULL) {
		avbox_queue_wake(inst->video_packets_q);
	}
	if (inst->audio_packets_q != NULL) {
		avbox_queue_wake(inst->audio_packets_q);
	}

	avbox_player_halt(inst);

	/* drop all decoded video frames */
	if (inst->video_stream_index != -1) {
		/* drop all video packets */
		while (avbox_queue_count(inst->video_packets_q) > 0) {
			av_packet = avbox_queue_get(inst->video_packets_q);
			av_packet_unref(av_packet->avpacket);
			release_av_packet(inst, av_packet);
		}

		/* Drop all decoded video frames */
		while (!inst->video_decoder_flushed ||
			avbox_queue_count(inst->video_frames_q) > 0) {
			if ((v_packet = avbox_queue_peek(inst->video_frames_q, 0)) != NULL) {
				if (avbox_queue_get(inst->video_frames_q) != v_packet) {
					LOG_PRINT_ERROR("Frame went missing!");
					abort();
				}
				if (v_packet->type == AVBOX_PLAYER_PACKET_TYPE_VIDEO) {
					av_frame_unref(v_packet->video_frame->avframe);
					release_av_frame(inst, v_packet->video_frame);
				}
				release_packet(inst, v_packet);
			} else {
				avbox_checkpoint_continue(&inst->video_decoder_checkpoint);
				usleep(50LL * 1000LL);
				avbox_checkpoint_halt(&inst->video_decoder_checkpoint);
				avbox_queue_wake(inst->video_packets_q);
			}
		}
	}

	/* drop all decoded audio frames */
	while (inst->audio_packets_q != NULL && (!inst->audio_decoder_flushed ||
		avbox_queue_count(inst->audio_packets_q) > 0)) {
		if ((av_packet = avbox_queue_peek(inst->audio_packets_q, 0)) != NULL) {
			if (avbox_queue_get(inst->audio_packets_q) != av_packet) {
				LOG_PRINT_ERROR("Packet went missing!");
				abort();
			}
			av_packet_unref(av_packet->avpacket);
			release_av_packet(inst, av_packet);
		} else {
			avbox_checkpoint_continue(&inst->audio_decoder_checkpoint);
			sched_yield();
			avbox_checkpoint_halt(&inst->audio_decoder_checkpoint);
			do {
				avbox_queue_wake(inst->audio_packets_q);
			} while (!avbox_checkpoint_wait(&inst->audio_decoder_checkpoint, 1000L));
		}
	}

	/* drop all decoded audio frames */
	if (inst->audio_stream != NULL) {
		avbox_audiostream_drop(inst->audio_stream);
		ASSERT(inst->audio_decoder_flushed);
	}

	DEBUG_VPRINT("player", "Audio time: %li",
		(inst->audio_stream == NULL) ? -1 : avbox_audiostream_gettime(inst->audio_stream));

	DEBUG_VPRINT("player", "Frames dropped. (time=%"PRIi64",v_packets=%i,a_packets=%i,v_frames=%i)",
		inst->getmastertime(inst),
		(inst->video_packets_q == NULL) ? 0 : avbox_queue_count(inst->video_packets_q),
		(inst->audio_packets_q == NULL) ? 0 : avbox_queue_count(inst->audio_packets_q),
		(inst->video_frames_q == NULL) ? 0 : avbox_queue_count(inst->video_frames_q));

	/* flush stream buffers */
	if (inst->fmt_ctx) {
		avformat_flush(inst->fmt_ctx);
	}

	/* clear still frame condition */
	avbox_player_clear_still_frame(inst);

	inst->flushing = 0;
	avbox_player_continue(inst);
}


/**
 * Check if the stream has underrun.
 */
static int
avbox_player_isunderrun(struct avbox_player * const inst)
{
	int underrun = 0;

	/* if playback has started but not ended then we're
	 * on underrun */
	if (inst->play_state != AVBOX_PLAYER_PLAYSTATE_PLAYING) {
		inst->underrun = 1;
		return 1;
	}

	if (!inst->stream_quit && !inst->stream_exiting) {
		if (inst->stream.self != NULL) {
			if (!inst->stream.underrun_expected(inst->stream.self) &&
				inst->stream.buffer_state != NULL) {
				int64_t count, capacity;
				inst->stream.buffer_state(inst->stream.self, &count, &capacity);
				if (inst->underrun) {
					if (count < capacity) {
						underrun = 1;
					}
				} else {
					if (count == 0) {
						underrun = 1;
					}
				}
			}
		}
		if (!underrun) {
			if (inst->underrun) {
				if (inst->audio_stream_index != -1) {
					if (avbox_audiostream_size(inst->audio_stream) < AVBOX_BUFFER_AUDIO) {
						underrun = 1;
					}
					if (inst->video_stream_index != -1) {
						/* the video buffer must be at least half full to
						 * exit underrun to ensure that the video decoder has
						 * started outputting frames at least */
						if (avbox_queue_count(inst->video_frames_q) < AVBOX_BUFFER_VIDEO / 2) {
							LOG_VPRINT_INFO("Still in underrun (video): %i of %i",
								avbox_queue_count(inst->video_frames_q), AVBOX_BUFFER_VIDEO);
							underrun = 1;
						}
					}
				} else {
					if (avbox_queue_count(inst->video_frames_q) < AVBOX_BUFFER_VIDEO) {
						underrun = 1;
					}
				}
			} else {
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
	tv.tv_sec = 1;
	tv.tv_nsec = 0;
	inst->underrun_timer_id = avbox_timer_register(&tv,
		AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
		avbox_thread_object(inst->control_thread), NULL, inst);
	if (inst->underrun_timer_id == -1) {
		avbox_player_throwexception(inst,
			"Could not start buffering timer");
		avbox_player_doresume(inst);
		avbox_player_updatestatus(inst,
			MB_PLAYER_STATUS_PLAYING);
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

	/* if the stream provider supports seeking use that */
	if (inst->stream.self != NULL && inst->stream.seek != NULL) {
		inst->stream.seek(inst->stream.self, flags, incr);
		return;
	}

	pos = inst->getmastertime(inst);

	if (flags & AVBOX_PLAYER_SEEK_CHAPTER) {

		int chapter = 0, chapter_pos = 0, nb_chapters = inst->fmt_ctx->nb_chapters;
		const int64_t chapter_duration = 60 * 5 * 1000L * 1000L;

		ASSERT(incr == -1 || incr == 1);

		/* find the current chapter */
		if (nb_chapters == 0) {
			int64_t p = 0;
			while (p < inst->fmt_ctx->duration) {
				if (p <= pos) {
					chapter = nb_chapters;
					chapter_pos = p;
				}
				p += chapter_duration;
				nb_chapters++;
			}

			DEBUG_VPRINT(LOG_MODULE, "Current chapter: %i",
				chapter);
		} else {
			for (chapter = 0; chapter < inst->fmt_ctx->nb_chapters; chapter++) {
				AVChapter*const ch = inst->fmt_ctx->chapters[chapter];
				if (av_compare_ts(pos, AV_TIME_BASE_Q,
					ch->start, ch->time_base) < 0) {

					chapter--;
					chapter_pos = av_rescale_q(
						inst->fmt_ctx->chapters[chapter]->start,
						inst->fmt_ctx->chapters[chapter]->time_base,
						AV_TIME_BASE_Q);
					break;
				}
			}
		}

		/* if we're more than 5 seconds into this chapter and we're seeking
		 * to the previous chapter then increment the chapter number so that
		 * seeking takes us back to the start of the chapter instead of the
		 * previous item on the playlist */
		if (incr == -1 && (pos - chapter_pos) > (5LL * 1000LL * 1000LL)) {
			chapter++;
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

	if (inst->underrun) {
		LOG_PRINT_ERROR("For now you can't seek while underrun. Sorry");
		return;
	}

	DEBUG_VPRINT("player", "Seeking (pos=%li, seek_to=%" PRIi64 ", offset=%" PRIi64 ")",
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

		DEBUG_PRINT(LOG_MODULE, "Halting stream input thread prior to SEEK");

		avbox_checkpoint_halt(&inst->stream_parser_checkpoint);
		do {
			if (inst->audio_packets_q != NULL) {
				avbox_queue_wake(inst->audio_packets_q);
			}
			if (inst->video_packets_q != NULL) {
				avbox_queue_wake(inst->video_packets_q);
			}
		} while (!avbox_checkpoint_wait(&inst->stream_parser_checkpoint, 10L * 1000L));

		DEBUG_VPRINT("player", "Seeking %s from %" PRIi64 " to %" PRIi64 "...",
			(flags & AVSEEK_FLAG_BACKWARD) ? "BACKWARD" : "FORWARD",
			seek_from, seek_to);

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

			DEBUG_VPRINT("player", "Seeking (newpos=%li)",
				inst->getmastertime(inst));

			DEBUG_PRINT("player", "Seek complete");
		}

		/* enter the underrun state and continue streaming */
		inst->underrun = 1;
		avbox_player_dopause(inst);
		avbox_player_handle_underrun(inst);
		avbox_checkpoint_continue(&inst->stream_parser_checkpoint);
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
		avbox_thread_object(inst->control_thread), NULL, inst);
	if (inst->stream_exit_timer_id == -1) {
		LOG_PRINT_ERROR("Could not start stream exit timer. Blocking thread!!");
		usleep(500L * 1000L);
		avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STREAM_EXIT, NULL);
	}
}


/**
 * Flush the player pipeline.
 */
static int
avbox_player_flush(struct avbox_player * const inst, const int flags)
{
	ASSERT(flags != AVBOX_PLAYER_FLUSH_INVALID);

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

			/* update the flushing flag and make sure the
			 * decoders are ware */
			inst->flushing = flags;
			if (inst->video_packets_q != NULL && (flags & AVBOX_PLAYER_FLUSH_VIDEO)) {
				avbox_queue_wake(inst->video_packets_q);
			}
			if (inst->audio_packets_q != NULL && (flags & AVBOX_PLAYER_FLUSH_AUDIO)) {
				avbox_queue_wake(inst->audio_packets_q);
			}
		}

		/* if there's any packets on the audio pipeline
		 * return 0 */
		if ((flags & AVBOX_PLAYER_FLUSH_AUDIO) && inst->play_state >= AVBOX_PLAYER_PLAYSTATE_AUDIODEC) {
			if (inst->audio_packets_q != NULL) {
				if (!inst->audio_decoder_flushed) {
					return 0;
				}
				if ((inst->audio_stream != NULL && avbox_audiostream_count(inst->audio_stream)) > 0 ||
					avbox_queue_count(inst->audio_packets_q) > 0) {
					return 0;
				}
			}
		}

		/* if there's any packets or frames on the video pipeline
		 * then rturn zero. */
		if ((flags & AVBOX_PLAYER_FLUSH_VIDEO) &&
			inst->play_state >= AVBOX_PLAYER_PLAYSTATE_VIDEODEC && inst->video_stream_index != -1) {
			if (inst->getmastertime == avbox_player_getsystemtime) {
				/* we're running by the video clock so there's no
				 * change of deadlock here so just return 0 until the
				 * video buffers are empty */
				if (avbox_queue_count(inst->video_packets_q) != 0 ||
					avbox_queue_count(inst->video_frames_q) != 0) {
					return 0;
				}
				if (!inst->video_decoder_flushed) {
					return 0;
				}

				/* make sure that the audio ring buffer is also flushed */
				avbox_audiostream_pause(inst->audio_stream);
				avbox_audiostream_resume(inst->audio_stream);

			} else {
				if (avbox_queue_count(inst->video_packets_q) > 0 ||
					avbox_queue_count(inst->video_frames_q) > 0 ||
					!inst->video_decoder_flushed) {
#ifndef NDEBUG
					struct avbox_av_frame *last_drop = NULL;
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

						struct avbox_player_packet *v_packet;
						avbox_checkpoint_halt(&inst->video_output_checkpoint);
						do {
							avbox_queue_wake(inst->video_frames_q);
						} while (!avbox_checkpoint_wait(&inst->video_output_checkpoint, 50L * 1000L));

						if (avbox_queue_count(inst->video_frames_q) > 0) {
							if ((v_packet = avbox_queue_peek(inst->video_frames_q, 0)) == NULL) {
								LOG_PRINT_ERROR("There appears to be data corruption. Aborting");
								abort();
							}
							if (v_packet->type == AVBOX_PLAYER_PACKET_TYPE_VIDEO) {
#ifndef NDEBUG
								if (last_drop != v_packet->video_frame) {
									DEBUG_VPRINT(LOG_MODULE, "Dumping frame with timestamp: %i",
										av_rescale_q(av_frame_get_best_effort_timestamp(v_packet->video_frame->avframe),
											inst->fmt_ctx->streams[inst->video_stream_index]->time_base,
											AV_TIME_BASE_Q));
									last_drop = v_packet->video_frame;
								}
#endif
								v_packet->video_frame->avframe->pts = AV_NOPTS_VALUE;
							}
						}

						avbox_checkpoint_continue(&inst->video_output_checkpoint);
						sched_yield(); /* let output thread run */
					}
				}
			}
		}
		
		if (flags & AVBOX_PLAYER_FLUSH_ALL) {
			/*avio_flush(inst->fmt_ctx->pb);*/
			avformat_flush(inst->fmt_ctx);
		}

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
				struct avbox_syncarg arg;
				avbox_syncarg_init(&arg, inst);
				ASSERT(inst->audio_decoder_worker == NULL);
				ASSERT(inst->audio_decoder_thread != NULL);
				if ((inst->audio_decoder_worker = avbox_thread_delegate(
					inst->audio_decoder_thread,
					avbox_player_audio_decode, &arg)) == NULL) {
					abort();
				}
				avbox_syncarg_wait(&arg);
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
				ASSERT(inst->video_decoder_worker == NULL);
				ASSERT(inst->video_decoder_thread != NULL);
#ifdef ENABLE_MMAL
				if (AV_CODEC_ID_H264 ==
					inst->fmt_ctx->streams[inst->video_stream_index]->codecpar->codec_id) {
					if ((inst->video_decoder_worker = avbox_thread_delegate(
						inst->video_decoder_thread, avbox_mmal_decode, inst)) == NULL) {
						LOG_VPRINT_ERROR("Could not start MMAL video decoder: %s",
							strerror(errno));
					}
				}
#endif
				if (inst->video_decoder_worker == NULL) {
					if ((inst->video_decoder_worker = avbox_thread_delegate(
						inst->video_decoder_thread, avbox_player_video_decode, inst)) == NULL) {
						LOG_VPRINT_ERROR("Could not start video decoder: %s",
							strerror(errno));
						avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
					}
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

			if (avbox_audiostream_start(inst->audio_stream) == -1) {
				ASSERT(errno != EEXIST);
				LOG_PRINT_ERROR("Could not start audio stream");
			}
			avbox_player_sendctl(inst, AVBOX_PLAYERCTL_AUDIOOUT_READY, NULL);
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
				ASSERT(inst->video_output_worker == NULL);
				ASSERT(inst->video_output_thread != NULL);
				if ((inst->video_output_worker = avbox_thread_delegate(
					inst->video_output_thread, avbox_player_video, inst)) == NULL) {
					LOG_VPRINT_ERROR("Could not start video output worker: %s",
						strerror(errno));
					avbox_player_sendctl(inst, AVBOX_PLAYERCTL_THREADEXIT, NULL);
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

			if (inst->stream.self != NULL) {
				if (inst->stream.must_flush_before_play) {
					avformat_flush(inst->fmt_ctx);
				}
				inst->stream.play(inst->stream.self, 0);
			}

			/* enter the underrun state to wait for
			 * the buffers to fill */
			inst->underrun = 1;
			avbox_player_dopause(inst);
			avbox_player_handle_underrun(inst);

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
			 * fast, otherwise wait for the pipeline to flush on it's
			 * own */
			if (inst->stopping) {
				DEBUG_PRINT(LOG_MODULE, "Dropping all frames");
				avbox_player_drop(inst);
			} else if (!avbox_player_flush(inst, AVBOX_PLAYER_FLUSH_ALL)) {
				avbox_player_delay_stream_exit(inst);
				release_ctlmsg(inst, ctlmsg);
				return AVBOX_DISPATCH_OK;
			}


			if (inst->video_stream_index != -1) {
				ASSERT(avbox_queue_count(inst->video_frames_q) == 0);
				ASSERT(avbox_queue_count(inst->video_packets_q) == 0);

				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_VIDEOOUT) {
					DEBUG_PRINT(LOG_MODULE, "Waiting for video output thread");
					avbox_delegate_wait(inst->video_output_worker, NULL);
					inst->video_output_worker = NULL;
				}
				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_VIDEODEC) {
					DEBUG_PRINT(LOG_MODULE, "Waiting for video decoder");
					avbox_delegate_wait(inst->video_decoder_worker, NULL);
					inst->video_decoder_worker = NULL;
				}

				avbox_queue_destroy(inst->video_frames_q);
				avbox_queue_destroy(inst->video_packets_q);
				inst->video_frames_q = NULL;
				inst->video_packets_q = NULL;
				inst->video_stream_index = -1;
			}

			/* cleanup audio decoder thread */
			if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_AUDIODEC) {
				if (inst->audio_decoder_worker != NULL) {
					DEBUG_PRINT(LOG_MODULE, "Waiting for audio decoder");
					avbox_delegate_wait(inst->audio_decoder_worker, NULL);
					inst->audio_decoder_worker = NULL;
				}
			}

			/* join the stream thread */
			DEBUG_PRINT(LOG_MODULE, "Waiting for stream input thread");
			avbox_delegate_wait(inst->stream_input_worker, NULL);
			inst->stream_input_worker = NULL;

			DEBUG_PRINT(LOG_MODULE, "Cleaning up");

			/* cleanup audio stuff */
			if (inst->audio_stream != NULL) {
				avbox_audiostream_destroy(inst->audio_stream);
				inst->audio_stream = NULL;
			}
			if (inst->audio_packets_q != NULL) {
				avbox_queue_destroy(inst->audio_packets_q);
				inst->audio_packets_q = NULL;
			}
			inst->audio_stream_index = -1;

			/* clean other stuff */
			if (inst->fmt_ctx != NULL) {
				avformat_close_input(&inst->fmt_ctx);
				/* avformat_free_context(inst->fmt_ctx); */ /* not sure if we need this */
				inst->fmt_ctx = NULL;
			}

			/* cleanup dvdnav stuff */
			if (inst->stream.self != NULL) {
				inst->stream.destroy(inst->stream.self);
				inst->stream.self = NULL;
			}

			avbox_player_updatestatus(inst, MB_PLAYER_STATUS_READY);

			/* if this is a playlist and the STOP wasn't requested
			 * then play the next item */
			if (!inst->stopping) {
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

			DEBUG_PRINT(LOG_MODULE, "Player stopped");

#ifdef DEBUG_MEMORY_POOLS
			LOG_VPRINT_INFO("Video packets leaked: %i", inst->packet_pool_allocs - LIST_SIZE(&inst->packet_pool));
			LOG_VPRINT_INFO("AVFrames leaked: %i", inst->frame_pool_allocs - LIST_SIZE(&inst->frame_pool));
			LOG_VPRINT_INFO("AVPackets leaked: %i", inst->av_packet_pool_allocs - LIST_SIZE(&inst->av_packet_pool));
			LOG_VPRINT_INFO("Control messages leaked: %i (1 is ok)", inst->ctlmsg_pool_allocs - LIST_SIZE(&inst->ctlmsg_pool));
#endif
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

			/* does the current stream allow pausing at this point? */
			if (inst->stream.self != NULL &&
				!inst->stream.can_pause(inst->stream.self)) {
				DEBUG_PRINT(LOG_MODULE, "Stream cannot be paused at this point");
				break;
			}

			/* update status and pause */
			avbox_player_dopause(inst);
			avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PAUSED);
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
			inst->stopping = 1;
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
		case AVBOX_PLAYERCTL_CHANGE_AUDIO_TRACK:
		{
			struct avbox_syncarg * const arg = ctlmsg->data;
			int stream, first_stream = -1, found_active = 0,
				next_stream = -1;

			DEBUG_VPRINT(LOG_MODULE, "AVBOX_PLAYERCTL_CHANGE_AUDIO_TRACK stream_id=%i",
				(arg == NULL) ? -2 : *(int*)avbox_syncarg_data(arg));

			if (arg == NULL) {
				for (stream = 0; stream < inst->fmt_ctx->nb_streams; stream++) {
					if (inst->fmt_ctx->streams[stream]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
						continue;
					}
					if (first_stream == -1) {
						first_stream = stream;
					}
					if (stream == inst->audio_stream_index) {
						found_active = 1;
						continue;
					}
					if (found_active) {
						next_stream = stream;
						break;
					}
				}
				if (next_stream == -1 && first_stream != -1) {
					next_stream = first_stream;
				}
			} else {
				int * const stream_id = avbox_syncarg_data(arg);
				if (*stream_id != -1) {
					for (stream = 0; stream < inst->fmt_ctx->nb_streams; stream++) {
						if (inst->fmt_ctx->streams[stream]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
							continue;
						}
						if (*stream_id == inst->fmt_ctx->streams[stream]->id) {
							next_stream = stream;
							/* found_active = 1; */
							break;
						}
					}
					if (!found_active) {
						DEBUG_VPRINT(LOG_MODULE, "Stream id: %d not found. It better come soon!",
							next_stream);
						inst->audio_stream_id = *stream_id;
						inst->audio_stream_index = -2;

						/* audio is master so make video buffer unbound */
						if (inst->video_stream_index != -1) {
							avbox_queue_setsize(inst->video_frames_q, AVBOX_BUFFER_VIDEO * 2);
						}

						/* we're done for now */
						avbox_syncarg_return(arg, NULL);
						break;
					}

					/* stream found, fall through */

				} else { /* no stream */
					DEBUG_PRINT(LOG_MODULE, "No audio stream. Switching to video clock");

					/* switch to the video clock */
					inst->getmastertime = avbox_player_getsystemtime;
					inst->audio_stream_index = -1;

					/* set video buffer limits */
					if (inst->video_stream_index != -1) {
						avbox_queue_setsize(inst->video_frames_q, AVBOX_BUFFER_VIDEO);
					}

					avbox_syncarg_return(arg, NULL);
					break;
				}
			}

			if (next_stream != -1 && next_stream != inst->audio_stream_index) {
				inst->audio_stream_index = next_stream;
			}

			break;
		}
		case AVBOX_PLAYERCTL_BUFFER_UNDERRUN:
		{
			if (inst->underrun) {
				break;
			}

			DEBUG_PRINT(LOG_MODULE, "Underrun message received");

			#if 0
			/* if the video pipeline is empty flush the decoder */
			if (inst->video_stream_index != -1) {
				if (!inst->flushing && !inst->video_decoder_flushed &&
					(avbox_queue_count(inst->video_frames_q) == 0 &&
					avbox_queue_count(inst->video_packets_q) == 0)) {
					DEBUG_PRINT(LOG_MODULE, "Video underrun detected. Flushing pipeline");
					inst->flushing = AVBOX_PLAYER_FLUSH_VIDEO;
					while (!inst->video_decoder_flushed) {
						avbox_queue_wake(inst->video_packets_q);
						sched_yield();
					}
					inst->flushing = 0;
				}
			}
			#endif

			/* underruns are expected while stopping or flushing
			 * no need to react */
			if (inst->flushing || inst->still_frame ||
				inst->stopping ||
				(inst->stream.self != NULL &&
					inst->stream.underrun_expected(inst->stream.self))) {
				break;
			}

			DEBUG_VPRINT(LOG_MODULE, "Checking underrun state=%i",
				inst->play_state);

			if (avbox_player_isunderrun(inst)) {
				DEBUG_VPRINT(LOG_MODULE, "Underrun detected (time=%li)!",
					inst->getmastertime(inst));
				inst->underrun = 1;
				if (inst->play_state >= AVBOX_PLAYER_PLAYSTATE_PLAYING) {
					avbox_player_dopause(inst);
				}
				avbox_player_handle_underrun(inst);
			}
			break;
		}
		case AVBOX_PLAYERCTL_AUDIO_STREAM_UNDERRUN:
		{
			DEBUG_PRINT(LOG_MODULE, "Audio stream underrun");
			LOG_PRINT_ERROR("Audio stream reported underrun");

			/* NOTE: This works but what if the video queue overruns the
			 * system memory before we can handle this? We need to ALWAYS limit
			 * the size of the video queue somehow */
			if (0 && !inst->stream_exiting && inst->play_state == AVBOX_PLAYER_PLAYSTATE_PLAYING) {
				if (inst->video_stream_index != -1) {
					if (avbox_queue_count(inst->audio_packets_q) == 0 &&
						avbox_audiostream_count(inst->audio_stream) == 0 &&
						avbox_queue_count(inst->video_frames_q) > 0) {
						DEBUG_PRINT(LOG_MODULE, "Audio stream dried!! Switching to video clock.");
						pthread_mutex_lock(&inst->state_lock);
						inst->getmastertime = avbox_player_getsystemtime;
						avbox_queue_setsize(inst->video_frames_q, AVBOX_BUFFER_VIDEO);
						pthread_mutex_unlock(&inst->state_lock);
					}
				}
			}

			if (0 && !inst->stream_exiting && inst->play_state == AVBOX_PLAYER_PLAYSTATE_PLAYING) {
				if (avbox_queue_count(inst->audio_packets_q) > 0 &&
					avbox_audiostream_count(inst->audio_stream) > 0 &&
					avbox_queue_count(inst->video_frames_q) == AVBOX_BUFFER_VIDEO) {

					/* NOTE: I think this was only needed to workaround a bug
					 * and may be safe to remove */
					DEBUG_PRINT(LOG_MODULE, "Audio stream dried!! Switching to video clock.");
					pthread_mutex_lock(&inst->state_lock);
					avbox_stopwatch_reset(inst->video_time,
						avbox_audiostream_gettime(inst->audio_stream));
					inst->getmastertime = avbox_player_getsystemtime;
					avbox_stopwatch_start(inst->video_time);
					pthread_mutex_unlock(&inst->state_lock);

				} else {
					/* TODO: Send AVBOX_PLAYERCTL_UNDERRUN? */
				}
			}
			break;
		}
		case AVBOX_PLAYERCTL_THREADEXIT:
		{
			LOG_VPRINT_ERROR("Thread exitted unexpectedly: play_state=%i",
				inst->play_state);

			if (inst->play_state != AVBOX_PLAYER_PLAYSTATE_READY &&
				!inst->stopping) {
				inst->stopping = 1;
				avbox_player_dostop(inst);
			}
			break;
		}
		case AVBOX_PLAYERCTL_FLUSH:
		{
			struct avbox_syncarg * const arg = ctlmsg->data;
			ASSERT(inst->play_state >= AVBOX_PLAYER_PLAYSTATE_PLAYING);

			if (inst->underrun_timer_id != -1) {
				LOG_PRINT_ERROR("Flushing while underrun!");
			}

			while (!avbox_player_flush(inst, AVBOX_PLAYER_FLUSH_ALL)) {
				usleep(10 * 1000L);
			}

			avbox_player_clear_still_frame(inst);
			avbox_syncarg_return(arg, NULL);
			break;
		}
		case AVBOX_PLAYERCTL_STILL_FRAME:
		{
			struct avbox_syncarg * const arg = ctlmsg->data;
			int length = (int) ((uintptr_t) avbox_syncarg_data(arg) & 0xff);
			if (length != 0xFF) {
				struct timespec wake;
				ASSERT(!inst->still_frame);
				ASSERT(inst->still_frame_timer_id == -1);

				DEBUG_VPRINT(LOG_MODULE, "AVBOX_PLAYER_STILL_FRAME %d", length);

				/* flush to make sure the frame is presented */
				while (!avbox_player_flush(inst, AVBOX_PLAYER_FLUSH_VIDEO)) {
					usleep(10L * 1000L);
				}

				wake.tv_sec = length;
				wake.tv_nsec = 0;
				inst->still_frame = 1;
				inst->still_frame_waiter = arg;
				inst->still_frame_timer_id = avbox_timer_register(&wake,
					AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
					avbox_thread_object(inst->control_thread), NULL, inst);
				if (inst->still_frame_timer_id == -1) {
					avbox_player_throwexception(inst,
						"Could not start still frame timer");
					avbox_syncarg_return(arg, NULL);
				}

			} else {
				if (!inst->still_frame) {
					DEBUG_VPRINT(LOG_MODULE, "AVBOX_PLAYERCTL_STILL_FRAME (length=%d)",
						length);

					/* flush to make sure the frame is presented */
					while (!avbox_player_flush(inst, AVBOX_PLAYER_FLUSH_VIDEO)) {
						usleep(100L * 1000L);
					}

					inst->still_frame = 1; /* will be cleared by FLUSH */
					inst->still_frame_waiter = arg;
				}
			}
			break;
		}
		case AVBOX_PLAYERCTL_SET_TITLE:
		{
			struct avbox_syncarg * const arg = ctlmsg->data;
			const char * const title = avbox_syncarg_data(arg);
			avbox_player_settitle(inst, title);
			avbox_player_updatestatus(inst, inst->status);
			avbox_syncarg_return(arg, NULL);
			break;
		}
		case AVBOX_PLAYERCTL_SET_DURATION:
		{
			struct avbox_syncarg * const arg = ctlmsg->data;
			const int64_t *duration = avbox_syncarg_data(arg);
			inst->state_info.duration = *duration;
			avbox_syncarg_return(arg, NULL);
			break;
		}
		case AVBOX_PLAYERCTL_SET_POSITION:
		{
			struct avbox_syncarg * const arg = ctlmsg->data;
			const int64_t *pos = avbox_syncarg_data(arg);
			inst->state_info.pos = *pos;
			avbox_syncarg_return(arg, NULL);
			break;
		}
		case AVBOX_PLAYERCTL_BUFFER_UPDATE:
		{
			if (inst->status == MB_PLAYER_STATUS_BUFFERING) {
				avbox_player_updatestatus(inst, MB_PLAYER_STATUS_BUFFERING);
			}
			break;
		}
		case AVBOX_PLAYERCTL_UPDATE:
		{
			struct avbox_delegate *del;
			struct avbox_player_updateargs args;
			args.inst = inst;
			args.frame = NULL;
			if ((del = avbox_application_delegate(avbox_player_doupdate, &args)) != NULL) {
				avbox_delegate_wait(del, NULL);
			}
			break;
		}
		default:
			DEBUG_VABORT("player", "Invalid message type: %i", ctlmsg->id);
			abort();
		}
		release_ctlmsg(inst, ctlmsg);
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
				inst->underrun = 0;
				inst->underrun_timer_id = -1;
				avbox_player_doresume(inst);
				avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);
				DEBUG_PRINT(LOG_MODULE, "Underrun cleared");
			}

		} else if (timer_data->id == inst->stream_exit_timer_id) {
			avbox_player_sendctl(inst, AVBOX_PLAYERCTL_STREAM_EXIT, NULL);
			inst->stream_exit_timer_id = -1;

		} else if (timer_data->id == inst->still_frame_timer_id) {
			inst->still_frame = 0;
			inst->still_frame_timer_id = -1;
			avbox_syncarg_return(inst->still_frame_waiter, NULL);

		} else {
			LOG_VPRINT_ERROR("Unexpected timer: %i", timer_data->id);
		}
		avbox_timers_releasepayload(timer_data);
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


/**
 * Tell the player to switch audio/subpicture tracks.
 */
void
avbox_player_changetrack(struct avbox_player * const inst,
	const int track_id, const int track_type)
{
	ASSERT(inst != NULL);
	ASSERT(track_id == -1); /* for now */
	if (track_type == AVBOX_PLAYER_AUDIO_TRACK) {
		avbox_player_sendctl(inst, AVBOX_PLAYERCTL_CHANGE_AUDIO_TRACK, NULL);
	} else if (track_type == AVBOX_PLAYER_SUBPX_TRACK) {
		/* not implemented */
		abort();
	} else {
		abort();
	}
}


unsigned int
avbox_player_bufferstate(struct avbox_player *inst)
{
	ASSERT(inst != NULL);
	if (inst->stream.self != NULL && inst->stream.buffer_state != NULL) {
		unsigned int percent;
		int64_t count, capacity;
		inst->stream.buffer_state(inst->stream.self, &count, &capacity);
		percent = (int) ((((count * 100.0) / (double) capacity) * 100.0) / 100.0);
		return percent;
	} else {
		DEBUG_VPRINT(LOG_MODULE, "percent=%i",
			inst->stream_percent);
		return inst->stream_percent;
	}
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

	/* if our local list is not empty then free it first */
	if (!LIST_EMPTY(&inst->playlist)) {
		avbox_player_freeplaylist(inst);
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
	char * path_copy = NULL;
	if (inst->playlist_item->filepath != NULL &&
		(path_copy = strdup(inst->playlist_item->filepath)) == NULL) {
		LOG_PRINT_ERROR("Could not allocate copy of play path!");
		return 0;
	}

	/* send the command to the player thread */
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_PLAY, path_copy);

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


static void*
avbox_player_set_control_thread_name(void * arg)
{
	(void) arg;
	DEBUG_SET_THREAD_NAME("player");
	return NULL;
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

		if (inst->play_state != AVBOX_PLAYER_PLAYSTATE_READY) {
			usleep(10L * 1000L);
			return AVBOX_DISPATCH_CONTINUE;
		}

		avbox_window_setdrawfunc(inst->window, NULL, NULL);
		avbox_player_freeplaylist(inst);

		if (inst->media_file != NULL) {
			free((void*) inst->media_file);
		}

		/* cleanup */
		avbox_thread_destroy(inst->control_thread);
		avbox_thread_destroy(inst->audio_decoder_thread);
		avbox_thread_destroy(inst->video_decoder_thread);
		avbox_thread_destroy(inst->video_output_thread);
		avbox_thread_destroy(inst->stream_input_thread);
		avbox_stopwatch_destroy(inst->video_time);
		break;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		struct avbox_av_packet *av_packet;
		struct avbox_av_frame *frame;
		struct avbox_player_packet *packet;
		struct avbox_player_ctlmsg *msg;

		DEBUG_PRINT("player", "Cleaning up after player");
		ASSERT(inst != NULL);

		/* free the pools */
		LIST_FOREACH_SAFE(struct avbox_av_packet*, av_packet, &inst->av_packet_pool, {
			LIST_REMOVE(av_packet);
			av_packet_free(&av_packet->avpacket);
			free(av_packet);
		});
		LIST_FOREACH_SAFE(struct avbox_av_frame*, frame, &inst->frame_pool, {
			LIST_REMOVE(frame);
			av_frame_free(&frame->avframe);
			free(frame);
		});
		LIST_FOREACH_SAFE(struct avbox_player_packet*, packet, &inst->packet_pool, {
			LIST_REMOVE(packet);
			free(packet);
		});
		LIST_FOREACH_SAFE(struct avbox_player_ctlmsg*, msg, &inst->ctlmsg_pool, {
			LIST_REMOVE(msg);
			free(msg);
		});

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


struct avbox_window *
avbox_player_window(struct avbox_player * const inst)
{
	return inst->window;
}


int
avbox_player_get_video_decode_cache_size(struct avbox_player * const inst)
{
	(void) inst;
	return AVBOX_BUFFER_VIDEO;
}


/**
 * Create a new player object.
 */
struct avbox_player*
avbox_player_new(struct avbox_window *window)
{
	struct avbox_player* inst;
	static int initialized = 0;
	pthread_mutexattr_t prio_inherit;

	/* initialize libav */
	if (!initialized) {
		int i, argc;
		const char **argv;

		for (i = 0, argv = avbox_application_args(&argc); i < argc; i++) {
			if (!strcmp(argv[i], "--avbox:decode_cache_size")) {
				if (++i < argc && strisdigit(argv[i])) {
					decode_cache_size = atoi(argv[i]);
				}
			}
		}

		av_register_all();
		avfilter_register_all();
		initialized = 1;

		/* set the malloc trim threshold to 256 MiB */
		mallopt(M_MMAP_MAX, 0);
		mallopt(M_TRIM_THRESHOLD, 1024 * 1024 * 256);
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

	/* initialize player threads */
	if ((inst->audio_decoder_thread = avbox_thread_new(NULL, NULL, AVBOX_THREAD_REALTIME, -4)) == NULL ||
		(inst->video_decoder_thread = avbox_thread_new(NULL, NULL, AVBOX_THREAD_REALTIME, -2)) == NULL ||
		(inst->video_output_thread = avbox_thread_new(NULL, NULL, AVBOX_THREAD_REALTIME, 0)) == NULL ||
		(inst->stream_input_thread = avbox_thread_new(NULL, NULL, AVBOX_THREAD_REALTIME, -4)) == NULL ||
		(inst->control_thread = avbox_thread_new(avbox_player_control, inst, AVBOX_THREAD_REALTIME, -5)) == NULL) {
		LOG_VPRINT_ERROR("Could not create threads: %s",
			strerror(errno));
		free(inst);
		abort();	/* until we cleanup */
		return NULL;
	} else {
		struct avbox_delegate *del;
		if ((del = avbox_thread_delegate(inst->control_thread,
			avbox_player_set_control_thread_name, NULL)) == NULL) {
			LOG_PRINT_ERROR("Could not set control thread name!");
		} else {
			avbox_delegate_wait(del, NULL);
		}
	}

	if ((inst->video_time = avbox_stopwatch_new()) == NULL) {
		LOG_PRINT_ERROR("Could not create stopwatch. Out of memory");
		free(inst);
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
	inst->still_frame_timer_id = -1;
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
	LIST_INIT(&inst->frame_pool);
	LIST_INIT(&inst->packet_pool);
	LIST_INIT(&inst->av_packet_pool);
	LIST_INIT(&inst->ctlmsg_pool);

	/* initialize pthreads primitives */
	pthread_mutexattr_init(&prio_inherit);
	pthread_mutexattr_setprotocol(&prio_inherit, PTHREAD_PRIO_INHERIT);
	if (pthread_mutex_init(&inst->state_lock, &prio_inherit) != 0 ||
		pthread_mutex_init(&inst->av_packet_pool_lock, &prio_inherit) != 0 ||
		pthread_mutex_init(&inst->packet_pool_lock, &prio_inherit) != 0 ||
		pthread_mutex_init(&inst->ctlmsg_pool_lock, &prio_inherit) != 0 ||
		pthread_mutex_init(&inst->frame_pool_lock, &prio_inherit) != 0) {
		LOG_PRINT_ERROR("Cannot create player instance. Pthreads error");
		avbox_object_destroy(inst->object);
		free(inst->state_info.title);
		free(inst);
		return NULL;
	}
	pthread_mutexattr_destroy(&prio_inherit);

	prime_pools(inst);

	/* initialize checkpoints */
	avbox_checkpoint_init(&inst->video_output_checkpoint);
	avbox_checkpoint_init(&inst->video_decoder_checkpoint);
	avbox_checkpoint_init(&inst->audio_decoder_checkpoint);
	avbox_checkpoint_init(&inst->stream_parser_checkpoint);

	avbox_window_setdrawfunc(inst->window, avbox_player_draw, inst);

	return inst;
}
