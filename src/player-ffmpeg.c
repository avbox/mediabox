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


/* for direct rendering */
#include <linux/fb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>


#include <alsa/asoundlib.h>


#include "video.h"
#include "player.h"
#include "debug.h"


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
#define MB_VIDEO_BUFFER_FRAMES	(3)
#define MB_AUDIO_BUFFER_FRAMES  (50)

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

	AVFormatContext *fmt_ctx;

	AVFrame *audio_frame[MB_AUDIO_BUFFER_FRAMES];
	char audio_frame_state[MB_AUDIO_BUFFER_FRAMES];
	int audio_playback_index;
	int audio_decode_index;
	int audio_frames;
	pthread_cond_t audio_signal;
	pthread_mutex_t audio_lock;
	pthread_t audio_thread;

	int video_stream_index;
	AVPacket video_packet;
	int video_packet_state;
	uint8_t *frame_data[MB_VIDEO_BUFFER_FRAMES];
	char frame_state[MB_VIDEO_BUFFER_FRAMES];
	int64_t frame_pts[MB_VIDEO_BUFFER_FRAMES];
	AVRational frame_time_base[MB_VIDEO_BUFFER_FRAMES];
	int frames_avail;
	int next_read_buf;
	int decode_frame_index;
	int renderer_quit;
	int audio_quit;
	pthread_cond_t video_decoder_signal;
	pthread_mutex_t video_decoder_lock;
	pthread_t video_decoder_thread;
	pthread_cond_t renderer_signal;
	pthread_mutex_t renderer_lock;
	pthread_t renderer_thread;

	pthread_cond_t resume_signal;
	pthread_mutex_t resume_lock;
	pthread_t thread;
};


#if (MB_VIDEO_BUFFER_FRAMES == 1)
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

void *
mbv_dfb_getscreenmask();



/**
 * mb_player_vrend_thread() -- Video rendering thread.
 */
#if (MB_VIDEO_BUFFER_FRAMES > 1)
static void *
mb_player_render(void *arg)
{
	uint8_t *buf;
	int64_t last_pts = AV_NOPTS_VALUE, frame_pts, delay;
	struct timespec last_real_pts, real_pts;
	struct mbp *inst = (struct mbp*) arg;
#ifdef MB_DECODER_PRINT_FPS
	struct timespec new_tp, last_tp, elapsed_tp;
	int frames = 0, fps = 0;
#endif
#ifdef MB_FBDEV_RENDERER
	int fd;
	long screensize;
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	void *fb_mem = NULL;
#endif

	MB_DEBUG_SET_THREAD_NAME("video_playback");

	assert(inst != NULL);

	fprintf(stderr, "mb_player: Renderer thread started\n");

	if (inst->use_fbdev) {
		if ((fd = open("/dev/fb0", O_RDWR)) != -1) {
			if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == -1 ||
				ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1) {
				fprintf(stderr, "mb_player[ffmpeg]: ioctl() failed. "
					"Direct rendering disabled\n");
				inst->use_fbdev = 0;
				close(fd);
			} else {
				screensize = vinfo.yres_virtual * finfo.line_length;
				fb_mem = mmap(0, screensize, PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, (off_t) 0);
				if (fb_mem == MAP_FAILED) {
					fprintf(stderr, "mb_player[ffmpeg]: mmap() failed. "
						"Direct rendering disabled\n");
					inst->use_fbdev = 0;
					close(fd);
				}
			}
		} else {
			inst->use_fbdev = 0;
		}
	}

	fprintf(stderr, "mb_player: Video renderer ready\n");

	/* signal control thread that we're ready */
	pthread_mutex_lock(&inst->renderer_lock);
	pthread_cond_signal(&inst->renderer_signal);
	pthread_mutex_unlock(&inst->renderer_lock);

#ifdef MB_DECODER_PRINT_FPS
	(void) clock_gettime(CLOCK_MONOTONIC, &last_tp);
	(void) clock_gettime(CLOCK_MONOTONIC, &new_tp);
#endif

	while (!inst->renderer_quit) {

		if (inst->action != MB_PLAYER_ACTION_NONE) {
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

		/* if there's no frame ready we must wait */
		if (inst->frame_state[inst->next_read_buf] != 1) {
			pthread_mutex_lock(&inst->renderer_lock);
			if (inst->renderer_quit) {
				pthread_mutex_unlock(&inst->renderer_lock);
				goto video_exit;
			}
			if (inst->frame_state[inst->next_read_buf] != 1) {
				/* fprintf(stderr, "mb_player[ffmpeg]: Waiting for decoder\n"); */
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
				int64_t elapsed;
				/* sleep roughly the right amount of time;
				 * usleep is in microseconds, just like AV_TIME_BASE. */
				delay = av_rescale_q(frame_pts - last_pts,
					inst->frame_time_base[inst->next_read_buf], AV_TIME_BASE_Q);
				(void) clock_gettime(CLOCK_MONOTONIC, &real_pts);
				elapsed = real_pts.tv_sec * 1000000000LL + real_pts.tv_nsec - 
					(last_real_pts.tv_sec * 1000000000LL + last_real_pts.tv_nsec);
				delay -= elapsed / 1000 + (elapsed % 1000 >= 500);
				if (delay > 0 && delay < 1000000) {
					usleep(delay);
				}
			}
			last_pts = frame_pts;
		}

		/* wait for vsync */
		unsigned int screen = 0;
		(void) ioctl(fd, FBIO_WAITFORVSYNC, &screen);

		/* save the last real pts */
		(void) clock_gettime(CLOCK_MONOTONIC, &last_real_pts);

		if (inst->use_fbdev) {
			int x, y, pixelsz;
			uint8_t *m = (uint8_t*) mbv_dfb_getscreenmask();

			pixelsz = vinfo.bits_per_pixel / CHAR_BIT;

			for (y = 0; y < vinfo.yres; y++) {
				for (x = 0; x < vinfo.xres; x++) {
					if (!m[(inst->width * y) + x]) {
						long location = (x+vinfo.xoffset) *
							pixelsz + (y+vinfo.yoffset) *
							finfo.line_length;
						uint32_t *ppix = (uint32_t*) buf;
						*((uint32_t*)(fb_mem + location)) = 
							*(ppix + (((inst->width * y) + x)));
					}
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
		fprintf(stderr, "Fps: %i | Video frames available: %i | Audio Frames: %i\r",
			fps, inst->frames_avail, inst->audio_frames);
#endif

		/* update buffer state and signal decoder */
		pthread_mutex_lock(&inst->renderer_lock);
		inst->frame_state[inst->next_read_buf] = 0;
		inst->next_read_buf++;
		inst->next_read_buf %= MB_VIDEO_BUFFER_FRAMES;
		inst->frames_avail--;
		pthread_cond_signal(&inst->renderer_signal);
		pthread_mutex_unlock(&inst->renderer_lock);
	}

video_exit:
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
 * mb_player_adec_thread() -- This is the main decoding loop
 */
static void*
mb_player_adec_thread(void *arg)
{
	int ret;
	struct mbp *inst = (struct mbp*) arg;
	const char *device = "default";
	snd_pcm_t *handle = NULL;
	snd_pcm_sframes_t frames;

	MB_DEBUG_SET_THREAD_NAME("audio_playback");

	assert(inst != NULL);
	assert(inst->audio_quit == 0);


	/* initialize alsa device */
	if ((ret = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_ASYNC)) < 0) {
		fprintf(stderr, "mb_player[ffmpeg]: snd_pcm_open() failed, ret=%i\n",
			ret);
		return NULL;
	}
	if ((ret = snd_pcm_set_params(handle,
		SND_PCM_FORMAT_S16,
		SND_PCM_ACCESS_RW_INTERLEAVED,
		2,
		48000,
		1,
		500000)) < 0) {
		fprintf(stderr, "mb_player[ffmpeg]: snd_pcm_set_params() failed. ret=%i\n", ret);
		snd_pcm_close(handle);
		return NULL;
	}

	/* signal video thread that we're ready to start */
	pthread_mutex_lock(&inst->audio_lock);
	pthread_cond_signal(&inst->audio_signal);
	pthread_mutex_unlock(&inst->audio_lock);

	fprintf(stderr, "Audio thread ready\n");

	/* start decoding */
	while (inst->audio_quit == 0) {

		/* if there's no frame ready we must wait */
		if (inst->audio_frame_state[inst->audio_playback_index] != 1) {
			pthread_mutex_lock(&inst->audio_lock);
			if (inst->audio_frame_state[inst->audio_playback_index] != 1) {
				/* fprintf(stderr, "Audio thread waiting\n"); */
				pthread_cond_wait(&inst->audio_signal, &inst->audio_lock);
				pthread_mutex_unlock(&inst->audio_lock);
				continue;
			}
			pthread_mutex_unlock(&inst->audio_lock);
		}

		/* play the frame */
		frames = snd_pcm_writei(handle, inst->audio_frame[inst->audio_playback_index]->data[0],
			inst->audio_frame[inst->audio_playback_index]->nb_samples);
		if (frames < 0) {
			frames = snd_pcm_recover(handle, frames, 0);
		}
		if (frames < 0) {
			fprintf(stderr, "mb_player[ffmpeg]: snd_pcm_writei() failed: %s\n",
				snd_strerror(frames));
			av_frame_unref(inst->audio_frame[inst->audio_playback_index]);
			goto audio_exit;
		}

		/* free frame */
		av_frame_unref(inst->audio_frame[inst->audio_playback_index]);

		/* update buffer state and signal decoder */
		pthread_mutex_lock(&inst->audio_lock);
		inst->audio_frame_state[inst->audio_playback_index] = 0;
		inst->audio_playback_index++;
		inst->audio_playback_index %= MB_AUDIO_BUFFER_FRAMES;
		inst->audio_frames--;
		pthread_cond_signal(&inst->audio_signal);
		pthread_mutex_unlock(&inst->audio_lock);
	}

audio_exit:
	fprintf(stderr, "mb_player[ffmpeg]: Audio thread exiting\n");

	/* cleanup */
	snd_pcm_close(handle);

	return NULL;
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
	fprintf(stderr, "mb_player[ffmpeg]: video_filters: %s\n",
		video_filters);
	if (mb_player_initvideofilters(inst->fmt_ctx, video_codec_ctx,
		&video_buffersink_ctx, &video_buffersrc_ctx, &video_filter_graph,
		video_filters, inst->video_stream_index) < 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not init filter graph!\n");
		goto decoder_exit;
	}

	/* calculate the size of each frame and allocate buffer for it */
	/* TODO: Make sure buffers are aligned */
	inst->bufsz = avpicture_get_size(MB_DECODER_PIX_FMT, inst->width, inst->height);
	inst->buf = buf = av_malloc(inst->bufsz * sizeof(uint8_t) * MB_VIDEO_BUFFER_FRAMES);
	if (buf == NULL) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not allocate buffer\n");
		goto decoder_exit;
	}

	for (i = 0; i < MB_VIDEO_BUFFER_FRAMES; i++) {
		inst->frame_data[i] = buf + (i * inst->bufsz);
		inst->frame_state[i] = 0;
	}

	fprintf(stderr, "mb_player[ffmpeg]: video_codec_ctx: width=%i height=%i pix_fmt=%i\n",
		inst->width, inst->height, video_codec_ctx->pix_fmt);

	/* allocate video frames */
	video_frame_nat = av_frame_alloc(); /* native */
	video_frame_flt = av_frame_alloc(); /* filtered */
	if (video_frame_nat == NULL || video_frame_flt == NULL) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not allocate frames\n");
		goto decoder_exit;
	}

	fprintf(stderr, "mb_player: Video decoder ready\n");

	/* signal control trhead that we're ready */
	pthread_mutex_lock(&inst->video_decoder_lock);
	pthread_cond_signal(&inst->video_decoder_signal);
	pthread_mutex_unlock(&inst->video_decoder_lock);

	while (!inst->renderer_quit) {
		/* if there's no frame to decode wait */
		if (inst->video_packet_state != 1) {
			pthread_mutex_lock(&inst->video_decoder_lock);
			if (inst->renderer_quit) {
				pthread_mutex_unlock(&inst->video_decoder_lock);
				goto decoder_exit;
			}
			if (inst->video_packet_state != 1) {
				pthread_cond_wait(&inst->video_decoder_signal, &inst->video_decoder_lock);
				pthread_mutex_unlock(&inst->video_decoder_lock);
				continue;
			}
			pthread_mutex_unlock(&inst->video_decoder_lock);
		}

		/* decode frame */
		if ((i = avcodec_decode_video2(video_codec_ctx, video_frame_nat, &finished, &inst->video_packet)) < 0) {
			fprintf(stderr, "mp_player: avcodec_decode_video2() returned %i\n", i);
		}

		if (finished) {
			/* free packet */
			av_free_packet(&inst->video_packet);

			pthread_mutex_lock(&inst->video_decoder_lock);
			inst->video_packet_state = 0;
			pthread_cond_signal(&inst->video_decoder_signal);
			pthread_mutex_unlock(&inst->video_decoder_lock);


			int64_t frame_pts = video_frame_nat->pts =
				av_frame_get_best_effort_timestamp(video_frame_nat);

			/* push the decoded frame into the filtergraph */
			if (av_buffersrc_add_frame_flags(video_buffersrc_ctx,
				video_frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
				fprintf(stderr, "mb_player[ffmpeg]: Error feeding filterchain\n");
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
				while (inst->frame_state[inst->decode_frame_index] != 0) {
					pthread_mutex_lock(&inst->renderer_lock);
					if (inst->renderer_quit) {
						pthread_mutex_unlock(&inst->renderer_lock);
						goto decoder_exit;
					}
					if (inst->frame_state[inst->decode_frame_index] != 0) {
						/*fprintf(stderr, "mb_player[ffmpeg]: "
							"Waiting for renderer\n"); */
						pthread_cond_wait(&inst->renderer_signal,
							&inst->renderer_lock);
					}
					pthread_mutex_unlock(&inst->renderer_lock);
					if (inst->renderer_quit) {
						goto decoder_exit;
					}
				}

				/* copy picture to buffer */
				avpicture_layout((const AVPicture*) video_frame_flt,
					MB_DECODER_PIX_FMT, inst->width, inst->height,
					inst->frame_data[inst->decode_frame_index], inst->bufsz);


				/* update the buffer index and signal renderer thread */
				pthread_mutex_lock(&inst->renderer_lock);
				inst->frame_state[inst->decode_frame_index] = 1;
				inst->frame_pts[inst->decode_frame_index] = frame_pts;
				inst->frame_time_base[inst->decode_frame_index] = 
					video_buffersink_ctx->inputs[0]->time_base;
				inst->decode_frame_index++;
				inst->decode_frame_index %= MB_VIDEO_BUFFER_FRAMES;
				inst->frames_avail++;
				pthread_cond_signal(&inst->renderer_signal);
				pthread_mutex_unlock(&inst->renderer_lock);

				av_frame_unref(video_frame_flt);
			}
			av_frame_unref(video_frame_nat);
		} else {
			pthread_mutex_unlock(&inst->video_decoder_lock);
		}
	}
decoder_exit:
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
 * mb_player_vdec_thread() -- This is the main decoding loop
 */
static void*
mb_player_stream_decode(void *arg)
{
	struct mbp *inst = (struct mbp*) arg;
	int i, finished;
	AVPacket packet, packet1;

	MB_DEBUG_SET_THREAD_NAME("stream_decoder");


	const char *audio_filters ="aresample=48000,aformat=sample_fmts=s16:channel_layouts=stereo";
	int audio_stream_index = -1;
	AVCodecContext *audio_codec_ctx = NULL;
	AVFrame *audio_frame_nat = NULL;
	AVFilterGraph *audio_filter_graph = NULL;
	AVFilterContext *audio_buffersink_ctx = NULL;
	AVFilterContext *audio_buffersrc_ctx = NULL;

	assert(inst != NULL);
	assert(inst->media_file != NULL);
	assert(inst->window != NULL);
	assert(inst->status == MB_PLAYER_STATUS_PLAYING);
	assert(inst->fmt_ctx == NULL);


	/* get the size of the window */
	if (mbv_window_getsize(inst->window, &inst->width, &inst->height) == -1) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not get window size\n");
		goto decoder_exit;
	}


	fprintf(stderr, "mb_player[ffmpeg]: Attempting to play (%ix%i) '%s'\n",
		inst->width, inst->height, inst->media_file);

	/* open file */
	if (avformat_open_input(&inst->fmt_ctx, inst->media_file, NULL, NULL) != 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not open '%s'\n",
			inst->media_file);
		goto decoder_exit;
	}

	if (avformat_find_stream_info(inst->fmt_ctx, NULL) < 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not find stream info\n");
		goto decoder_exit;
	}

	/* fire the video decoder thread */
	pthread_mutex_lock(&inst->video_decoder_lock);
	if (pthread_create(&inst->video_decoder_thread, NULL, mb_player_video_decode, inst) != 0) {
		abort();
	}
	pthread_cond_wait(&inst->video_decoder_signal, &inst->video_decoder_lock);
	pthread_mutex_unlock(&inst->video_decoder_lock);
	fprintf(stderr, "mb_player: Video stream: %i\n", inst->video_stream_index);


	/* dump file info */
	av_dump_format(inst->fmt_ctx, 0, inst->media_file, 0);

	inst->renderer_quit = 0;
	inst->next_read_buf = 0;
	inst->decode_frame_index = 0;
	inst->frames_avail = 0;
	inst->video_packet_state = 0;

	/* we're ready to start decoding, but first let us fire
	 * the rendering thread */
	pthread_mutex_lock(&inst->renderer_lock);
	if (pthread_create(&inst->renderer_thread, NULL, mb_player_render, inst) != 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not start renderer thread\n");
		pthread_mutex_unlock(&inst->renderer_lock);
		goto decoder_exit;
	}
	pthread_cond_wait(&inst->renderer_signal, &inst->renderer_lock);
	pthread_mutex_unlock(&inst->renderer_lock);


	/* open the audio codec */
	if (open_codec_context(&audio_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
		audio_codec_ctx = inst->fmt_ctx->streams[audio_stream_index]->codec;	
	}
	if (audio_codec_ctx == NULL) {
		goto decoder_exit;
	}

	/* allocate audio frames */
	audio_frame_nat = av_frame_alloc(); /* native */
	if (audio_frame_nat == NULL) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not allocate audio frames\n");
		goto decoder_exit;
	}

	/* initialize video filter graph */
	fprintf(stderr, "mb_player[ffmpeg]: audio_filters: %s\n",
		audio_filters);
	if (mb_player_initaudiofilters(inst->fmt_ctx, audio_codec_ctx,
		&audio_buffersink_ctx, &audio_buffersrc_ctx, &audio_filter_graph,
		audio_filters, audio_stream_index) < 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not init filter graph!\n");
		goto decoder_exit;
	}

	/* allocate filtered frames */
	inst->audio_decode_index = 0;
	inst->audio_playback_index = 0;
	inst->audio_frames = 0;
	inst->audio_quit = 0;

	for (i = 0; i < MB_AUDIO_BUFFER_FRAMES; i++) {
		inst->audio_frame[i] = av_frame_alloc();
		inst->audio_frame_state[i] = 0;
		assert(inst->audio_frame[i] != NULL);
	}

	/* start audio thread and wait until it's ready to play */
	pthread_mutex_lock(&inst->audio_lock);
	if (pthread_create(&inst->audio_thread, NULL, mb_player_adec_thread, inst) != 0) {
		abort();
	}
	pthread_cond_wait(&inst->audio_signal, &inst->audio_lock);
	pthread_mutex_unlock(&inst->audio_lock);

	/* start decoding */
	while (av_read_frame(inst->fmt_ctx, &packet) >= 0) {
		if (packet.stream_index == inst->video_stream_index) {
			/* wait for the decoder to finish decoding the current frame */
			while (inst->video_packet_state == 1) {
				pthread_mutex_lock(&inst->video_decoder_lock);
				if (inst->video_packet_state == 1) {
					pthread_cond_wait(&inst->video_decoder_signal, &inst->video_decoder_lock);
					pthread_mutex_unlock(&inst->video_decoder_lock);
					if (inst->action & MB_PLAYER_ACTION_STOP) {
						goto decoder_exit;
					}
					continue;
				}
			}
			
			/* save the packet and signal decoder thread */
			inst->video_packet = packet;
			//av_packet_ref(&inst->video_packet, &packet);
			inst->video_packet_state = 1;
			pthread_cond_signal(&inst->video_decoder_signal);
			pthread_mutex_unlock(&inst->video_decoder_lock);

		} else if (packet.stream_index == audio_stream_index) {

			packet1 = packet;

			/* decode audio frame */
			while (packet1.size > 0) {
				finished = 0;
				i = avcodec_decode_audio4(audio_codec_ctx, audio_frame_nat,
					&finished, &packet1);
				if (i < 0) {
					av_log(NULL, AV_LOG_ERROR, "Error decoding audio\n");
					continue;
				}
				packet1.size -= i;
				packet1.data += i;

				if (finished) {
			                /* push the audio data from decoded frame into the filtergraph */
					if (av_buffersrc_add_frame_flags(audio_buffersrc_ctx,
						audio_frame_nat, 0) < 0) {
						av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph\n");
						break;
					}

					/* pull filtered audio from the filtergraph */
					while (1) {

						/* if the renderer has not finished we must wait */
						while (inst->audio_frame_state[inst->audio_decode_index] != 0) {
							pthread_mutex_lock(&inst->audio_lock);
							if (inst->audio_frame_state[inst->audio_decode_index] != 0) {
								/*fprintf(stderr, "mb_player[ffmpeg]: "
									"Decoder waiting for audio thread\n"); */
								pthread_cond_wait(&inst->audio_signal,
									&inst->audio_lock);
							}
							pthread_mutex_unlock(&inst->audio_lock);
						}

						i = av_buffersink_get_frame(audio_buffersink_ctx,
							inst->audio_frame[inst->audio_decode_index]);
						if (i == AVERROR(EAGAIN) || i == AVERROR_EOF) {
							av_frame_unref(inst->audio_frame[inst->audio_decode_index]);
							break;
						}
						if (i < 0) {
							av_frame_unref(inst->audio_frame[inst->audio_decode_index]);
							goto decoder_exit;
						}

						/* update the buffer index and signal renderer thread */
						pthread_mutex_lock(&inst->audio_lock);
						inst->audio_frame_state[inst->audio_decode_index] = 1;
						inst->audio_decode_index++;
						inst->audio_decode_index %= MB_AUDIO_BUFFER_FRAMES;
						inst->audio_frames++;
						pthread_cond_signal(&inst->audio_signal);
						pthread_mutex_unlock(&inst->audio_lock);

					}
				}
			}
			/* free packet */
			av_free_packet(&packet);
		}


	}

decoder_exit:
	fprintf(stderr, "mb_player[ffmpeg]: Decoder exiting\n");

	/* clear the screen */
	pthread_mutex_lock(&inst->renderer_lock);
	memset(inst->buf, 0, inst->bufsz);
	for (i = 0; i < MB_VIDEO_BUFFER_FRAMES; i++) {
		inst->frame_state[i] = 1;
	}
	pthread_cond_signal(&inst->renderer_signal);
	pthread_mutex_unlock(&inst->renderer_lock);

	/* signal the renderer thread to exit and join it */
	pthread_mutex_lock(&inst->renderer_lock);
	inst->renderer_quit = 1;
	pthread_cond_signal(&inst->renderer_signal);
	pthread_mutex_unlock(&inst->renderer_lock);
	pthread_join(inst->renderer_thread, NULL);

	/* signal the video decoder thread to exit and join it */
	pthread_mutex_lock(&inst->video_decoder_lock);
	inst->action |= MB_PLAYER_ACTION_STOP;
	pthread_cond_signal(&inst->video_decoder_signal);
	pthread_mutex_unlock(&inst->video_decoder_lock);
	pthread_join(inst->video_decoder_thread, NULL);
	inst->action &= ~MB_PLAYER_ACTION_STOP;

	/* signal and wait for the audio thread to exit */
	pthread_mutex_lock(&inst->audio_lock);
	inst->audio_quit = 1;
	pthread_cond_signal(&inst->audio_signal);
	pthread_mutex_unlock(&inst->audio_lock);
	pthread_join(inst->audio_thread, NULL);

	/* clean audio stuff */
	for (i = 0; i < MB_AUDIO_BUFFER_FRAMES; i++) {
		av_free(inst->audio_frame[i]);
	}
	if (audio_frame_nat != NULL) {
		av_free(audio_frame_nat);
	}
	if (audio_codec_ctx != NULL) {
		avcodec_close(audio_codec_ctx);
	}

	/* clean video stuff */
	if (inst->fmt_ctx != NULL) {
		avformat_close_input(&inst->fmt_ctx);
		inst->fmt_ctx = NULL;
	}

	inst->video_stream_index = -1;
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
	if (pthread_create(&inst->thread, NULL, mb_player_stream_decode, inst) != 0) {
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
		fprintf(stderr, "mb_player: Cannot puase, not playing\n");
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
	/* if the video is paused then unpause it first. */
	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		mbp_play(inst, NULL);
	}

	if (inst->status != MB_PLAYER_STATUS_READY) {
		inst->action |= MB_PLAYER_ACTION_STOP;
		/* TODO: Don't spin */
		while (inst->status != MB_PLAYER_STATUS_READY);
		return 0;
	}
	return -1;
}


static void
mb_player_checkfbdev(struct mbp *inst)
{
	gid_t gid;
	uid_t uid;
	int root_gained = 0, fd;

	assert(inst != NULL);
	assert(inst->window != NULL);

	if (!mbv_isfbdev()) {
		fprintf(stderr, "mb_player[ffmpeg]: WARNING!!: Direct rendering disabled\n");
		inst->use_fbdev = 0;
		return;
	}

	fprintf(stderr, "mb_player[ffmpeg]: Initializing /dev/fb0\n");

	/* try to gain root */
	if (geteuid() != 0 && getuid() == 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Gaining root rights\n");
		gid = getegid();
		uid = geteuid();
		if (seteuid(0) == -1 || setegid(0) == -1) {
			fprintf(stderr, "mb_player[ffmpeg]: Could not get root rights\n");
		} else {
			root_gained = 1;
		}
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
			fprintf(stderr, "mb_player[ffmpeg]: ioctl() failed\n");
			inst->use_fbdev = 0;
			goto end;
		}

		/* dump some screen info */
		fprintf(stderr, "mb_player[ffmpeg]: bpp=%i\n", vinfo.bits_per_pixel);
		fprintf(stderr, "mb_player[ffmpeg]: type=%i\n", finfo.type);
		fprintf(stderr, "mb_player[ffmpeg]: visual=%i\n", finfo.visual);
		fprintf(stderr, "mb_player[ffmpeg]: FOURCC (grayscale): '%c%c%c%c'\n",
			((char*)&vinfo.grayscale)[0], ((char*)&vinfo.grayscale)[1],
			((char*)&vinfo.grayscale)[2], ((char*)&vinfo.grayscale)[3]);
		fprintf(stderr, "mb_player[ffmpeg]: xoffset=%i yoffset=%i r=%i g=%i b=%i\n"
			"mb_player[ffmpeg]: r=%i g=%i b=%i\n",
			vinfo.xoffset, vinfo.yoffset,
			vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset,
			vinfo.red.length, vinfo.green.length, vinfo.blue.length);

		/* try to mmap video memory */
		screensize = vinfo.yres_virtual * finfo.line_length;
		fb_mem = mmap(0, screensize, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, (off_t) 0);
		if (fb_mem == MAP_FAILED) {
			fprintf(stderr, "mb_player[ffmpeg]: mmap() failed\n");
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
	if (root_gained) {
		if (seteuid(uid) == -1 || setegid(gid) == -1) {
			fprintf(stderr, "mb_player[ffmpeg]: WARNING!!: Could not drop root rights!\n");
		}
	}
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
	inst->fmt_ctx = NULL;
	inst->bufsz = 0;
	inst->use_fbdev = 1;
	inst->video_stream_index = -1;
	inst->action = MB_PLAYER_ACTION_NONE;
	inst->status = MB_PLAYER_STATUS_READY;

	/* initialize pthreads primitives */
	if (pthread_mutex_init(&inst->resume_lock, NULL) != 0 ||
#if (MB_VIDEO_BUFFER_FRAMES > 1)
		pthread_mutex_init(&inst->renderer_lock, NULL) != 0 ||
		pthread_mutex_init(&inst->audio_lock, NULL) != 0 ||
		pthread_mutex_init(&inst->video_decoder_lock, NULL) != 0 ||
		pthread_cond_init(&inst->video_decoder_signal, NULL) != 0 ||
		pthread_cond_init(&inst->audio_signal, NULL) != 0 ||
		pthread_cond_init(&inst->renderer_signal, NULL) != 0 ||
#endif
		pthread_cond_init(&inst->resume_signal, NULL) != 0) {
		fprintf(stderr, "mb_player[ffmpeg]: pthreads initialization failed\n");
		free(inst);
		return NULL;
	}

	/* check if the framebuffer device is usable for
	 * direct rendering */
	mb_player_checkfbdev(inst);

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

