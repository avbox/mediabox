#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

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

#include <linux/fb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "video.h"
#include "player.h"


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
#define MB_DECODER_BUFFER_FRAMES	(3)

#define MB_DECODER_PRINT_FPS


/* render directly to fbdev */
#define MB_FBDEV_RENDERER


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
	uint8_t *buf;
	int bufsz;
	uint8_t *render_mask;
	int use_fbdev;
#if (MB_DECODER_BUFFER_FRAMES > 1)
	uint8_t *frame_data[MB_DECODER_BUFFER_FRAMES];
	int frame_state[MB_DECODER_BUFFER_FRAMES];
	int64_t frame_pts[MB_DECODER_BUFFER_FRAMES];
	AVRational frame_time_base[MB_DECODER_BUFFER_FRAMES];
	int frames_avail;
	int next_read_buf;
	int decode_frame_index;
	int renderer_quit;
	pthread_cond_t renderer_signal;
	pthread_mutex_t renderer_lock;
	pthread_t renderer_thread;
#else
	int64_t last_pts;
	int64_t frame_pts;
	AVRational time_base;
#endif
	pthread_cond_t resume_signal;
	pthread_mutex_t resume_lock;
	pthread_t thread;
};


static int fbdev;

#if (MB_DECODER_BUFFER_FRAMES == 1)
static void
mb_player_renderframe(struct mbp *inst)
{
	int64_t delay;

	assert(inst != NULL);
	assert(inst->window != NULL);
	abort();

	if  (inst->frame_pts != AV_NOPTS_VALUE) {
		if (inst->last_pts != AV_NOPTS_VALUE) {
			/* sleep roughly the right amount of time;
			 * usleep is in microseconds, just like AV_TIME_BASE. */
			delay = av_rescale_q(inst->frame_pts - inst->last_pts,
				inst->time_base, AV_TIME_BASE_Q);
			if (delay > 0 && delay < 1000000) {
				usleep(delay);
			}
		}
		inst->last_pts = inst->frame_pts;
	}

	mbv_window_blit_buffer(inst->window, inst->buf,
		inst->width, inst->height, 0, 0);
	inst->frames_rendered++;
}
#endif


#ifdef MB_DECODER_PRINT_FPS
static struct timespec
timediff(struct timespec *start, struct timespec *end)
{
	struct timespec temp;
	if ((end->tv_nsec - start->tv_nsec)<0) {
		temp.tv_sec = end->tv_sec - start->tv_sec - 1;
		temp.tv_nsec = 1000000000 + end->tv_nsec - start->tv_nsec;
	} else {
		temp.tv_sec = end->tv_sec - start->tv_sec;
		temp.tv_nsec = end->tv_nsec - start->tv_nsec;
	}
	return temp;
}
#endif


/**
 * mb_player_vrend_thread() -- Video rendering thread.
 */
#if (MB_DECODER_BUFFER_FRAMES > 1)
static void *
mb_player_render(void *arg)
{
	uint8_t *buf;
	int64_t last_pts = AV_NOPTS_VALUE, frame_pts, delay;
	struct mbp *inst = (struct mbp*) arg;
#ifdef MB_DECODER_PRINT_FPS
	struct timespec new_tp, last_tp, elapsed_tp;
	int frames = 0, fps = 0;
#endif
#ifdef MB_FBDEV_RENDERER
	int bytes_per_pixel = -1;
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	void *fb_mem = NULL;
#endif

	assert(inst != NULL);

	if (inst->use_fbdev) {
		if ((fbdev = open("/dev/fb0", O_RDWR)) != -1) {
			long screensize;
			ioctl(fbdev, FBIOGET_VSCREENINFO, &vinfo);
			ioctl(fbdev, FBIOGET_FSCREENINFO, &finfo);
			fprintf(stderr, "mb_player[ffmpeg]: bpp=%i\n",
				vinfo.bits_per_pixel);
			bytes_per_pixel = vinfo.bits_per_pixel / 8;
			screensize = vinfo.yres_virtual * finfo.line_length;
			fb_mem = mmap(0, screensize, PROT_READ | PROT_WRITE,
				MAP_SHARED, fbdev, (off_t) 0);
			fprintf(stderr, "byts_per_pixel = %i xoffset=%i yoffset=%i\n",
				bytes_per_pixel, vinfo.xoffset, vinfo.yoffset);



int x,y;

	for (x=0;x<vinfo.xres;x++)
		for (y=0;y<vinfo.yres;y++)
		{
			long location = (x+vinfo.xoffset) * (vinfo.bits_per_pixel/8) + (y+vinfo.yoffset) * finfo.line_length;
			*((uint32_t*)(fb_mem + location)) = 0xFFFFFFFF;
		}

		} else {
			inst->use_fbdev = 0;
		}
	}

#ifdef MB_DECODER_PRINT_FPS
	(void) clock_gettime(CLOCK_MONOTONIC, &last_tp);
	(void) clock_gettime(CLOCK_MONOTONIC, &new_tp);
#endif

	while (!inst->renderer_quit) {
		/* if there's no frame ready we must wait */
		if (inst->frame_state[inst->next_read_buf] != 1) {
			pthread_mutex_lock(&inst->renderer_lock);
			if (inst->frame_state[inst->next_read_buf] != 1) {
				/*fprintf(stderr, "mb_player[ffmpeg]: Waiting for decoder\n");*/
				pthread_cond_wait(&inst->renderer_signal, &inst->renderer_lock);
				pthread_mutex_unlock(&inst->renderer_lock);
				continue;
			}
			pthread_mutex_unlock(&inst->renderer_lock);
		}

		frame_pts = inst->frame_pts[inst->next_read_buf];
		buf = inst->frame_data[inst->next_read_buf];

		if  (frame_pts != AV_NOPTS_VALUE) {
			if (last_pts != AV_NOPTS_VALUE) {
				/* sleep roughly the right amount of time;
				 * usleep is in microseconds, just like AV_TIME_BASE. */
				delay = av_rescale_q(frame_pts - last_pts,
					inst->frame_time_base[inst->next_read_buf], AV_TIME_BASE_Q);
				if (delay > 0 && delay < 1000000) {
					usleep(delay);
				}
			}
			last_pts = frame_pts;
		}

		if (inst->use_fbdev) {
			//memcpy(fb_mem, buf, inst->width * inst->height * bytes_per_pixel);
			int x,y;

			for (y = 0;y < vinfo.yres; y++) {
				for (x=0;x<vinfo.xres;x++) {
					long location = (x+vinfo.xoffset) * (vinfo.bits_per_pixel/8) + (y+vinfo.yoffset) * finfo.line_length;
					*((uint32_t*)(fb_mem + location)) = 
						*(buf + (((inst->width * y) + x) * bytes_per_pixel));
				}
			}
		} else  {
			/* blit the frame */
			mbv_window_blit_buffer(inst->window, buf,
				inst->width, inst->height, 0, 0);
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
		fprintf(stderr, "Fps: %i | Frames available: %i\r",
			fps, inst->frames_avail);
#endif

		/* update buffer state and signal decoder */
		pthread_mutex_lock(&inst->renderer_lock);
		inst->frame_state[inst->next_read_buf] = 0;
		inst->next_read_buf++;
		inst->next_read_buf %= MB_DECODER_BUFFER_FRAMES;
		inst->frames_avail--;
		pthread_cond_signal(&inst->renderer_signal);
		pthread_mutex_unlock(&inst->renderer_lock);
	}

	return NULL;
	
}
#endif


/**
 * mb_player_initfilters() -- Initialize ffmpeg's filter graph
 */
static int
mb_player_initfilters(
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

void *
mbv_dfb_getscreenmask();

/**
 * mb_player_vdec_thread() -- This is the main decoding loop
 */
static void*
mb_player_vdec_thread(void *arg)
{
	int i, stream_index, finished;
	uint8_t *buf = NULL;
	struct mbp *inst = (struct mbp*) arg;
	char filters[512];
	AVFormatContext *fmt_ctx = NULL;;
	AVCodecContext *codec_ctx_orig = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVCodec *codec = NULL;
	AVFilterGraph *filter_graph = NULL;
	AVFrame *frame_nat = NULL, *frame_rgb = NULL, *frame_flt = NULL;
	AVFilterContext *buffersink_ctx = NULL;
	AVFilterContext *buffersrc_ctx = NULL;
	AVPacket packet;

	assert(inst != NULL);
	assert(inst->media_file != NULL);
	assert(inst->window != NULL);
	assert(inst->status == MB_PLAYER_STATUS_PLAYING);


	/* get the size of the window */
	if (mbv_window_getsize(inst->window, &inst->width, &inst->height) == -1) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not get window size\n");
		goto decoder_exit;
	}


	fprintf(stderr, "mb_player[ffmpeg]: Attempting to play (%ix%i) '%s'\n",
		inst->width, inst->height, inst->media_file);

	/* open file */
	if (avformat_open_input(&fmt_ctx, inst->media_file, NULL, NULL) != 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not open '%s'\n",
			inst->media_file);
		goto decoder_exit;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not find stream info\n");
		goto decoder_exit;
	}

	if (open_codec_context(&stream_index, fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
		codec_ctx = fmt_ctx->streams[stream_index]->codec;	
	}
	if (codec_ctx == NULL) {
		goto decoder_exit;
	}

	/* dump file info */
	av_dump_format(fmt_ctx, 0, inst->media_file, 0);

	/* find decoder */
	codec = avcodec_find_decoder(codec_ctx->codec_id);
	if (codec == NULL) {
		fprintf(stderr, "mb_player[ffmpeg]: Unsupported codec!\n");
		goto decoder_exit;
	}

	/* initialize filter graph */
	snprintf(filters, sizeof(filters), "scale='if(gt(a,4/3),%i,-1)':'if(gt(a,4/3),-1,%i)',"
		"pad=%i:%i:'((out_w - in_w) / 2)':'((out_h - in_h) / 2)'",
		inst->width, inst->height, inst->width, inst->height);
	fprintf(stderr, "mb_player[ffmpeg]: filters: %s\n",
		filters);
	if (mb_player_initfilters(fmt_ctx, codec_ctx,
		&buffersink_ctx, &buffersrc_ctx, &filter_graph,
		filters, stream_index) < 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not init filter graph!\n");
		goto decoder_exit;
	}

	/* allocate frames */
	frame_nat = av_frame_alloc(); /* native */
	frame_flt = av_frame_alloc(); /* filtered */
	if (frame_nat == NULL || frame_flt == NULL) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not allocate frames\n");
		goto decoder_exit;
	}

	/* calculate the size of each frame and allocate buffer for it */
	inst->bufsz = avpicture_get_size(MB_DECODER_PIX_FMT, inst->width, inst->height);
	inst->buf = buf = av_malloc(inst->bufsz * sizeof(uint8_t) * MB_DECODER_BUFFER_FRAMES);
	if (buf == NULL) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not allocate buffer\n");
		goto decoder_exit;
	}

	fprintf(stderr, "mb_player[ffmpeg]: codec_ctx: width=%i height=%i pix_fmt=%i\n",
		inst->width, inst->height, codec_ctx->pix_fmt);

#if (MB_DECODER_BUFFER_FRAMES == 1)
	inst->last_pts = AV_NOPTS_VALUE;
#else
	inst->renderer_quit = 0;
	inst->next_read_buf = 0;
	inst->decode_frame_index = 0;
	inst->frames_avail = 0;

	for (i = 0; i < MB_DECODER_BUFFER_FRAMES; i++) {
		inst->frame_data[i] = buf + (i * inst->bufsz);
		inst->frame_state[i] = 0;
	}

	/* we're ready to start decoding, but first let us fire
	 * the rendering thread */
	if (pthread_create(&inst->renderer_thread, NULL, mb_player_render, inst) != 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not start renderer thread\n");
		pthread_cond_wait(&inst->renderer_signal, &inst->renderer_lock);
		pthread_mutex_unlock(&inst->renderer_lock);
		goto decoder_exit;
	}
#endif

	/* start decoding */
	while (av_read_frame(fmt_ctx, &packet) >= 0) {
		if (packet.stream_index == stream_index) {
			/* decode frame */
			avcodec_decode_video2(codec_ctx, frame_nat, &finished, &packet);
			if (finished) {

				int64_t frame_pts = frame_nat->pts =
					av_frame_get_best_effort_timestamp(frame_nat);

				/* push the decoded frame into the filtergraph */
				if (av_buffersrc_add_frame_flags(buffersrc_ctx,
					frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
					fprintf(stderr, "mb_player[ffmpeg]: Error feeding filterchain\n");
					goto decoder_exit;
				}

				/* pull filtered frames from the filtergraph */
				while (1) {
					i = av_buffersink_get_frame(buffersink_ctx, frame_flt);
					if (i == AVERROR(EAGAIN) || i == AVERROR_EOF) {
						break;
					}
					if (i < 0) {
						goto decoder_exit;
					}

#if (MB_DECODER_BUFFER_FRAMES > 1)
					/* if the renderer has not finished we must wait */
					while (inst->frame_state[inst->decode_frame_index] != 0) {
						pthread_mutex_lock(&inst->renderer_lock);
						if (inst->frame_state[inst->decode_frame_index] != 0) {
							/* fprintf(stderr, "mb_player[ffmpeg]: "
								"Waiting for renderer\n"); */
							pthread_cond_wait(&inst->renderer_signal,
								&inst->renderer_lock);
						}
						pthread_mutex_unlock(&inst->renderer_lock);
					}

					/* copy picture to buffer */
					avpicture_layout((const AVPicture*) frame_flt,
						MB_DECODER_PIX_FMT, inst->width, inst->height,
						inst->frame_data[inst->decode_frame_index], inst->bufsz);

					uint8_t *m = (uint8_t*) mbv_dfb_getscreenmask();
					uint32_t *p = (uint32_t*) inst->frame_data[inst->decode_frame_index];
					int bufsz = inst->width * inst->height;
					for (i = 0; i < bufsz; i++) {
						if (m[i]) {
							p[i] = 0;
						}
					}

					/* update the buffer index and signal renderer thread */
					pthread_mutex_lock(&inst->renderer_lock);
					inst->frame_state[inst->decode_frame_index] = 1;
					inst->frame_pts[inst->decode_frame_index] = frame_pts;
					inst->frame_time_base[inst->decode_frame_index] = 
						buffersink_ctx->inputs[0]->time_base;
					inst->decode_frame_index++;
					inst->decode_frame_index %= MB_DECODER_BUFFER_FRAMES;
					inst->frames_avail++;
					pthread_cond_signal(&inst->renderer_signal);
					pthread_mutex_unlock(&inst->renderer_lock);

#else
					/* copy picture to buffer */
					avpicture_layout((const AVPicture*) frame_flt,
						MB_DECODER_PIX_FMT, inst->width, inst->height,
						inst->buf, inst->bufsz);

					/* get timebase */
					inst->frame_pts = frame_pts;
					inst->time_base = buffersink_ctx->inputs[0]->time_base;

					/* render frame */
					mb_player_renderframe(inst);
#endif
					av_frame_unref(frame_flt);
				}
				av_frame_unref(frame_nat);
			}
		}
		/* free packet */
		av_free_packet(&packet);

		if (inst->action != MB_PLAYER_ACTION_NONE) {

			if (inst->action & MB_PLAYER_ACTION_STOP) {
				inst->action &= ~MB_PLAYER_ACTION_STOP;
				goto decoder_exit;
			}

			/* this is where we pause -- not done yet */
			if (inst->action & MB_PLAYER_ACTION_PAUSE) {
				fprintf(stderr, "decoder: pausing\n");
				pthread_mutex_lock(&inst->resume_lock);
				inst->action &= ~MB_PLAYER_ACTION_PAUSE;
				inst->status = MB_PLAYER_STATUS_PAUSED;
				pthread_cond_wait(&inst->resume_signal, &inst->resume_lock);
				inst->status = MB_PLAYER_STATUS_PLAYING;
				pthread_mutex_unlock(&inst->resume_lock);
			}
		}
	}

decoder_exit:
	fprintf(stderr, "mb_player[ffmpeg]: Decoder exiting\n");

#if (MB_DECODER_BUFFER_FRAMES > 1)
	/* clear the screen */
	pthread_mutex_lock(&inst->renderer_lock);
	memset(inst->buf, 0, inst->bufsz);
	for (i = 0; i < MB_DECODER_BUFFER_FRAMES; i++) {
		inst->frame_state[i] = 1;
	}
	pthread_cond_signal(&inst->renderer_signal);
	pthread_mutex_unlock(&inst->renderer_lock);
#else
	memset(inst->buf, 0, inst->bufsz);
	mb_player_renderframe(inst);
#endif


#if (MB_DECODER_BUFFER_FRAMES > 1)
	/* signal the renderer thread to exit and join it */
	pthread_mutex_lock(&inst->renderer_lock);
	inst->renderer_quit = 1;
	pthread_cond_signal(&inst->renderer_signal);
	pthread_mutex_unlock(&inst->renderer_lock);
	pthread_join(inst->renderer_thread, NULL);
#endif


	/* cleanup */
	if (buf != NULL) {
		av_free(buf);
		inst->buf = NULL;
	}
	if (frame_rgb != NULL) {
		av_free(frame_rgb);
	}
	if (frame_nat != NULL) {
		av_free(frame_nat);
	}
	if (codec_ctx != NULL) {
		avcodec_close(codec_ctx);
	}
	if (codec_ctx_orig != NULL) {
		avcodec_close(codec_ctx_orig);
	}
	if (fmt_ctx != NULL) {
		avformat_close_input(&fmt_ctx);
	}

	inst->action = MB_PLAYER_ACTION_NONE;
	inst->status = MB_PLAYER_STATUS_READY;

	return NULL;
}


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


/**
 * mb_player_play() -- If path is not NULL it opens the file
 * specified by path and starts playing it. If path is NULL
 * it resumes playback if we're on the PAUSED state and return
 * failure code (-1) if we're on any other state.
 */
int 
mbp_play(struct mbp *inst, const char * const path)
{
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
			pthread_cond_signal(&inst->resume_signal);
			pthread_mutex_unlock(&inst->resume_lock);
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
	inst->status = MB_PLAYER_STATUS_PLAYING;

	/* start the main decoder thread */
	if (pthread_create(&inst->thread, NULL, mb_player_vdec_thread, inst) != 0) {
		fprintf(stderr, "pthread_create() failed!\n");
		inst->status = MB_PLAYER_STATUS_READY;
		return -1;
	}

	/* detach the decoder thread */
	pthread_detach(inst->thread);

	return 0;
}


int
mbp_pause(struct mbp* inst)
{
	assert(inst != NULL);

	/* can't pause if we're not playing */
	if (inst->status != MB_PLAYER_STATUS_PLAYING) {
		return -1;
	}

	/* request pause and wait for player thread to pause */
	inst->action |= MB_PLAYER_ACTION_PAUSE;

	/* TODO: Don't spin */
	while (inst->status != MB_PLAYER_STATUS_PAUSED);
	return 0;
}


int
mbp_stop(struct mbp* inst)
{
	if (inst->status != MB_PLAYER_STATUS_READY) {
		inst->action |= MB_PLAYER_ACTION_STOP;
		/* TODO: Don't spin */
		while (inst->status != MB_PLAYER_STATUS_READY);
		return 0;
	}
	return -1;
}


/**
 * mb_player_init() -- Create a new player object.
 */
struct mbp*
mbp_init(void)
{
	struct mbp* inst;
	struct mbv_window *window = NULL; /* TODO: This should be an argument */
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
			fprintf(stderr, "mb_player[ffmpeg]: Could not get root window\n");
			free(inst);
			return NULL;
		}
	}

	inst->window = window;
	inst->media_file = NULL;
	inst->buf = NULL;
	inst->bufsz = 0;
	inst->use_fbdev = 1;
	inst->action = MB_PLAYER_ACTION_NONE;
	inst->status = MB_PLAYER_STATUS_READY;

	/* initialize pthreads primitives */
	if (pthread_mutex_init(&inst->resume_lock, NULL) != 0 ||
#if (MB_DECODER_BUFFER_FRAMES > 1)
		pthread_mutex_init(&inst->renderer_lock, NULL) != 0 ||
		pthread_cond_init(&inst->renderer_signal, NULL) != 0 ||
#endif
		pthread_cond_init(&inst->resume_signal, NULL) != 0) {
		fprintf(stderr, "mb_player[ffmpeg]: pthreads initialization failed\n");
		free(inst);
		return NULL;
	}

	return inst;
}


void
mbp_destroy(struct mbp *inst)
{
	assert(inst != NULL);

	fprintf(stderr, "mb_player[ffmpeg]: Destroying\n");

	/* this just fails if we're not playing */
	(void) mbp_stop(inst);

	if (inst->media_file != NULL) {
		free(inst);
	}
}

