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


#ifndef __FFMPEG_UTIL__
#define __FFMPEG_UTIL__

/*
 * For now to change the pix format it needs to be
 * done here and on video-directfb.c avbox_window_blitbuf()
 * function. We need to implement our own enum with supported
 * formats (on video.h) and add it as an argument to that
 * function. Then use a LUT to map between those and ffmpeg's.
 */
//#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_RGB565)
//#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_RGB32)
#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_BGRA)
//#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_YUV420)

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include "ui/video.h"


static inline enum avbox_pixel_format
avbox_pixfmt_from_libav(enum AVPixelFormat pix_fmt)
{
	switch (pix_fmt) {
	case AV_PIX_FMT_YUV420P: return AVBOX_PIXFMT_YUV420P;
	case AV_PIX_FMT_BGRA: return AVBOX_PIXFMT_BGRA;
	default: return AVBOX_PIXFMT_UNKNOWN;
	}
}


static inline enum AVPixelFormat
avbox_pixfmt_to_libav(enum avbox_pixel_format pix_fmt)
{
	switch (pix_fmt) {
	case AVBOX_PIXFMT_UNKNOWN: return 0;
	case AVBOX_PIXFMT_BGRA: return AV_PIX_FMT_BGRA;
	case AVBOX_PIXFMT_YUV420P: return AV_PIX_FMT_YUV420P;
	case AVBOX_PIXFMT_MMAL: return 0;
	}
	return 0;
}


/**
 * Initialize ffmpeg's filter graph
 */
int
avbox_ffmpegutil_initvideofilters(
	AVFormatContext *fmt_ctx,
	AVCodecContext *dec_ctx,
	AVFilterContext **buffersink_ctx,
	AVFilterContext **buffersrc_ctx,
	AVFilterGraph **filter_graph,
	enum AVPixelFormat pix_fmt,
	const char *filters_descr,
	int stream_index);


int
avbox_ffmpegutil_initaudiofilters(
	AVFilterContext **buffersink_ctx,
	AVFilterContext **buffersrc_ctx,
	AVFilterGraph **filter_graph,
	const char *filters_descr,
	int sample_rate,
	AVRational time_base,
	uint64_t channel_layout,
	const char *sample_fmt_name);


AVCodecContext *
avbox_ffmpegutil_opencodeccontext(int *stream_idx,
	AVFormatContext *fmt_ctx, enum AVMediaType type);

#endif
