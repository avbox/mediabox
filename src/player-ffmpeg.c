#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>


#include "input.h"
#include "player.h"

struct mbp
{
	const char *media_file;
	enum mb_player_status status;
	int pause_requested;
	int frames_rendered;
	uint8_t *buf;
	pthread_t thread;
};


static void
mb_player_render_frame(struct mbp *inst)
{
	inst->frames_rendered++;
	return;
}


static int open_codec_context(int *stream_idx,
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
 * mb_player_playback_thread() -- This is the main decoding loop
 * TODO: Split into decoding and rendering threads
 */
static void*
mb_player_playback_thread(void *arg)
{
	int i, stream_index, finished;
	uint8_t *buf = NULL;
	struct mbp *inst = (struct mbp*) arg;
	struct SwsContext *sws_ctx = NULL;
	AVFormatContext *fmt_ctx = NULL;;
	AVCodecContext *codec_ctx_orig = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVCodec *codec = NULL;
	AVFrame *frame_nat = NULL, *frame_rgb = NULL;
	AVPacket packet;

	assert(inst != NULL);
	assert(inst->media_file != NULL);

	fprintf(stderr, "mb_player[ffmpeg]: Attempting to play '%s'\n",
		inst->media_file);

	/* open file */
	if (avformat_open_input(&fmt_ctx, inst->media_file, NULL, NULL) != 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not open '%s'\n",
			inst->media_file);
		goto decoder_exit;
	}

	#if 1
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


	#else
	/* dump file info */
	av_dump_format(fmt_ctx, 0, inst->media_file, 0);

	/* find the first video stream */
	for (i = 0; i < fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			fprintf(stderr, "mb_player[ffmpeg]: Found stream #%i\n", i);
			codec_ctx_orig = fmt_ctx->streams[i]->codec;
			stream_index = i;
			break;
		}
	}
	if (codec_ctx_orig == NULL) {
		goto decoder_exit;
	}

	/* find decoder */
	codec = avcodec_find_decoder(codec_ctx_orig->codec_id);
	if (codec == NULL) {
		fprintf(stderr, "mb_player[ffmpeg]: Unsupported codec!\n");
		goto decoder_exit;
	}

	/* copy context to our codec */
	codec_ctx = avcodec_alloc_context3(codec);
	if (avcodec_copy_context(codec_ctx, codec_ctx_orig) != 0) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not copy codec context\n");
		goto decoder_exit;
	}
	#endif


	/* allocate frames */
	frame_nat = av_frame_alloc();
	frame_rgb = av_frame_alloc();
	if (frame_nat == NULL || frame_rgb == NULL) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not allocate frames\n");
		goto decoder_exit;
	}

	/* calculate the size of each frame and allocate buffer for it */
	i = avpicture_get_size(PIX_FMT_RGB24, codec_ctx->width, codec_ctx->height);
	inst->buf = buf = av_malloc(i * sizeof(uint8_t));
	if (buf == NULL) {
		fprintf(stderr, "mb_player[ffmpeg]: Could not allocate buffer\n");
		goto decoder_exit;
	}

	/* assign buffer to frame */
	avpicture_fill((AVPicture*) frame_rgb, buf, PIX_FMT_RGB24,
		codec_ctx->width, codec_ctx->height);

	fprintf(stderr, "mb_player[ffmpeg]: codec_ctx: width=%i height=%i pix_fmt=%i\n",
		codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt);

	/* init the software scaler */
	sws_ctx = sws_getContext(
		codec_ctx->width,
		codec_ctx->height,
		codec_ctx->pix_fmt,
		codec_ctx->width,
		codec_ctx->height,
		PIX_FMT_RGB24,
		SWS_PRINT_INFO,
		NULL,
		NULL,
		NULL); 

	while (av_read_frame(fmt_ctx, &packet) >= 0) {
		if (packet.stream_index == stream_index) {
			/* decode frame */
			avcodec_decode_video2(codec_ctx, frame_nat, &finished, &packet);
			if (finished) {
				/* convert to rgb */
				sws_scale(sws_ctx, (uint8_t const * const *) frame_nat->data,
					frame_nat->linesize, 0, codec_ctx->height,
					frame_rgb->data, frame_rgb->linesize);

				/* render frame */
				mb_player_render_frame(inst);
				fprintf(stderr, "Rendered\n");

			}
		}
		/* free packet */
		av_free_packet(&packet);

		/* this is where we pause -- not done yet */
		if (inst->pause_requested) {
			inst->status = MB_PLAYER_STATUS_PAUSED;
			sleep(10);
			inst->status = MB_PLAYER_STATUS_PLAYING;
		}
	}

decoder_exit:
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

	/* this is a hack for now to get the media player window to update */
	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		mbp_play(inst, NULL);
		while (inst->status == MB_PLAYER_STATUS_PAUSED);
		mbp_pause(inst);
	}
}


int 
mbp_play(struct mbp *inst, const char * const path)
{
	if (inst == NULL) {
		return -1;
	}

	if (path == NULL) {
		if (inst->status == MB_PLAYER_STATUS_PAUSED) {
			/* just signal decoder and exit */
			return 0;
		}
		fprintf(stderr, "mbp_play() failed -- NULL path\n");
		return -1;
	}

	inst->media_file = path;

	if (pthread_create(&inst->thread, NULL, mb_player_playback_thread, inst) != 0) {
		fprintf(stderr, "pthread_create() failed!\n");
		return -1;
	}
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
	inst->pause_requested = 1;
	while (inst->status != MB_PLAYER_STATUS_PAUSED);
	return 0;
}


int
mbp_stop(struct mbp* inst)
{
	return 0;
}


struct mbp*
mbp_init(void)
{
	struct mbp* inst;
	static int initialized = 0;

	if (!initialized) {
		av_register_all();
		initialized = 1;
	}

	inst = malloc(sizeof(struct mbp));
	if (inst == NULL) {
		fprintf(stderr, "mbp_init() failed -- out of memory\n");
		return NULL;
	}

	inst->media_file = NULL;
	inst->pause_requested = 0;
	inst->buf = NULL;
	inst->status = MB_PLAYER_STATUS_READY;
	return inst;
}


void
mbp_destroy(struct mbp *inst)
{
	if (inst == NULL) {
		return;
	}
	if (inst->media_file != NULL) {
		free(inst);
	}
}

