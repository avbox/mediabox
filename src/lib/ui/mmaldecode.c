#include <stdint.h>

#if 0
#	include "/opt/vc/include/interface/mmal/mmal.h"
#	include "/opt/vc/include/interface/mmal/mmal_parameters.h"
#	include "/opt/vc/include/interface/mmal/util/mmal_connection.h"
#	include "/opt/vc/include/interface/mmal/util/mmal_util.h"
#	include "/opt/vc/include/interface/mmal/util/mmal_util_params.h"
#	include "/opt/vc/include/interface/mmal/util/mmal_default_components.h"
#else
#	include <interface/mmal/mmal.h>
#	include <interface/mmal/mmal_parameters.h>
#	include <interface/mmal/util/mmal_connection.h>
#	include <interface/mmal/util/mmal_util.h>
#	include <interface/mmal/util/mmal_util_params.h>
#	include <interface/mmal/util/mmal_default_components.h>
#endif

#define LOG_MODULE "mmaldecode"

#include "../avbox.h"
#include "../compiler.h"
#include "player_p.h"

#define ENABLE_ZERO_COPY 1

#define N_EXTRA_BUFFERS	(16)

struct avbox_mmal_context
{
	MMAL_PORT_T *input;
	MMAL_PORT_T *output;
	MMAL_POOL_T *input_pool;
	MMAL_POOL_T *output_pool;
	MMAL_ES_FORMAT_T *output_format;
	struct avbox_player * inst;
	int time_set;
	int flushing;
	int in_transit;		/* frames decoded but not yet presented */
	int in_decoder;		/* packets currently being decoded */
};

struct avbox_mmal_frame
{
	struct avbox_mmal_context *ctx;
	MMAL_BUFFER_HEADER_T *buffer;
};


static void
avbox_mmal_control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	struct avbox_mmal_context * const ctx = (struct avbox_mmal_context*) port->userdata;

	(void)ctx;

	if (buffer->cmd == MMAL_EVENT_ERROR) {
		MMAL_STATUS_T status = *(uint32_t*) buffer->data;
		LOG_VPRINT_ERROR("MMAL error %d: %s",
			status, mmal_status_to_string(status));
	}

	mmal_buffer_header_release(buffer);
}


static void
avbox_mmal_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	struct avbox_mmal_context * const ctx = (struct avbox_mmal_context*) port->userdata;
	AVPacket * const packet = buffer->user_data;

	/* return buffer header to pool */
	buffer->user_data = NULL;
	mmal_buffer_header_release(buffer);

	/* free the packet */
	if (packet != NULL) {
		av_packet_unref(packet);
		free(packet);
	}

	ATOMIC_DEC(&ctx->in_decoder);
}


static void
avbox_mmal_release_frame_buffer(void *opaque, uint8_t *data);


static int
avbox_mmal_send_output_buffer(struct avbox_mmal_context * const ctx)
{
	MMAL_STATUS_T status;
	MMAL_BUFFER_HEADER_T *buffer;
	struct avbox_mmal_frame * mmal_frame;
	AVFrame *frame;

	/* get a buffer from the output pool */
	if ((buffer = mmal_queue_get(ctx->output_pool->queue)) == NULL) {
		LOG_PRINT_ERROR("Could not get MMAL output buffer");
		return -1;
	}

	/* Allocate an AVFrame structure */
	if ((frame = av_frame_alloc()) == NULL) {
		ABORT("Out of memory");
	}

#ifdef ENABLE_ZERO_COPY
	/* save opaque pointer in AVFrame */
	frame->data[0] = buffer->data;
#else
	/* fill the YUV plane pointers */
	frame->data[0] = buffer->data;
	frame->data[1] = frame->data[0] +
		(ctx->output->format->es->video.width * ctx->output->format->es->video.height);
	frame->data[2] = frame->data[1] +
		((ctx->output->format->es->video.width * ctx->output->format->es->video.height) >> 2);
	frame->linesize[0] = ctx->output->format->es->video.width;
	frame->linesize[1] = ctx->output->format->es->video.width >> 1;
	frame->linesize[2] = frame->linesize[1];
#endif

	/* allocate a buffer for the buffer header pointer */
	if ((mmal_frame = malloc(sizeof(struct avbox_mmal_frame))) == NULL) {
		ABORT("Out of memory");
	}
	mmal_frame->ctx = ctx;
	mmal_frame->buffer = buffer;

	frame->buf[0] = av_buffer_create((void*) mmal_frame, sizeof(struct avbox_mmal_frame),
		avbox_mmal_release_frame_buffer, NULL, AV_BUFFER_FLAG_READONLY);
	if (frame->buf[0] == NULL) {
		ABORT("Out of memory");
	}

	/* initialize mmal buffer hdr with the new frame structure */
	mmal_buffer_header_reset(buffer);
	buffer->user_data = frame;
	buffer->cmd = 0;

	/* send it to the output port */
	if ((status = mmal_port_send_buffer(ctx->output, buffer)) != MMAL_SUCCESS) {
		LOG_VPRINT_ERROR("Could not send output buffer: %s",
			mmal_status_to_string(status));
		av_frame_unref(frame);
		return -1;
	}

	return 0;
}


static void
avbox_mmal_output_port_fill(struct avbox_mmal_context * const ctx)
{
	int i;
	const unsigned int avail =
		mmal_queue_length(ctx->output_pool->queue);
	for (i = 0; i < avail; i++) {
		if (avbox_mmal_send_output_buffer(ctx) < 0) {
			break;
		}
	}
}


static void
avbox_mmal_release_frame_buffer(void *opaque, uint8_t *data)
{
	struct avbox_mmal_frame * const frame = (void*) data;
	mmal_buffer_header_release(frame->buffer);
	if (!frame->ctx->flushing) {
		avbox_mmal_output_port_fill(frame->ctx);
	}
	ATOMIC_DEC(&frame->ctx->in_transit);
	free(data);
}


static void
avbox_mmal_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	struct avbox_mmal_context * const ctx = (struct avbox_mmal_context*) port->userdata;

	if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED) {
		MMAL_EVENT_FORMAT_CHANGED_T * const ev =
			mmal_event_format_changed_get(buffer);
		MMAL_ES_FORMAT_T *fmt;

		DEBUG_PRINT(LOG_MODULE, "MMAL format changed");

		if ((fmt = mmal_format_alloc()) == NULL) {
			ABORT("Could not allocate MMAL format");
		}

		mmal_format_full_copy(fmt, ev->format);
#ifdef ENABLE_ZERO_COPY
		fmt->encoding = MMAL_ENCODING_OPAQUE;
#endif
		ctx->output_format = fmt;
		mmal_buffer_header_release(buffer);

	} else if (buffer->cmd == 0) {
		if (buffer->length > 0) {
			struct avbox_player_packet *v_packet;
			AVFrame * const frame = buffer->user_data;

			frame->pts = buffer->pts;
			frame->pkt_dts = AV_NOPTS_VALUE;

			if (!ctx->time_set) {

				DEBUG_VPRINT(LOG_MODULE, "Sending clock reset message: %"PRIi64,
					frame->pts);

				if ((v_packet = malloc(sizeof(struct avbox_player_packet))) == NULL) {
					LOG_VPRINT_ERROR("Could not allocate clock packet: %s",
						strerror(errno));
					abort();
				}

				v_packet->type = AVBOX_PLAYER_PACKET_TYPE_SET_CLOCK;
				v_packet->clock_value = frame->pts;
				ctx->time_set = 1;

				while (avbox_queue_put(ctx->inst->video_frames_q, v_packet) == -1) {
					if (errno == EAGAIN) {
						continue;
					} else if (errno == ESHUTDOWN) {
						LOG_PRINT_ERROR("Video frames queue closed unexpectedly!");
					} else {
						LOG_VPRINT_ERROR("Error: avbox_queue_put() failed: %s",
							strerror(errno));
					}
					mmal_buffer_header_release(buffer);
					av_frame_unref(frame);
					av_free(frame);
					free(v_packet);
					return;
				}
			}

			/* allocate packet */
			if ((v_packet = malloc(sizeof(struct avbox_player_packet))) == NULL) {
				LOG_VPRINT_ERROR("Could not allocate clock packet: %s",
					strerror(errno));
				abort();
			}

			v_packet->type = AVBOX_PLAYER_PACKET_TYPE_VIDEO;
			v_packet->video_frame = frame;

			/* add frame to decoded frames queue */
			while (avbox_queue_put(ctx->inst->video_frames_q, v_packet) == -1) {
				if (errno == EAGAIN) {
					continue;
				} else if (errno == ESHUTDOWN) {
					LOG_PRINT_ERROR("Video frames queue closed unexpectedly!");
				} else {
					LOG_VPRINT_ERROR("Error: avbox_queue_put() failed: %s",
						strerror(errno));
				}
				mmal_buffer_header_release(buffer);
				av_frame_unref(frame);
				av_free(frame);
				free(v_packet);
				return;
			}

			ATOMIC_INC(&ctx->in_transit);

		} else {
			buffer->user_data = NULL;
			buffer->alloc_size = 0;
			buffer->data = NULL;
			mmal_buffer_header_release(buffer);
		}
	} else {
		mmal_buffer_header_release(buffer);
	}
}


static inline int
avbox_mmal_output_format_change(struct avbox_player * const inst,
	struct avbox_mmal_context * const ctx)
{
	MMAL_STATUS_T status;
	int ret = -1;

	DEBUG_PRINT(LOG_MODULE, "Changing output format");

	/* disable port */
	if ((status = mmal_port_disable(ctx->output)) != MMAL_SUCCESS) {
		LOG_VPRINT_ERROR("Could not disable MMAL output port: %s",
			mmal_status_to_string(status));
		goto end;
	}

#ifdef ENABLE_ZERO_COPY
	/* enable zero-copy */
	if ((status = mmal_port_parameter_set_boolean(ctx->output,
		MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE)) != MMAL_SUCCESS) {
		LOG_VPRINT_ERROR("Could not enable zero copy!: %s",
			mmal_status_to_string(status));
		goto end;
	}
#endif

#if 1
	if ((status = mmal_port_parameter_set_uint32(ctx->output,
		MMAL_PARAMETER_EXTRA_BUFFERS, N_EXTRA_BUFFERS)) != MMAL_SUCCESS) {
		LOG_VPRINT_ERROR("Could not set extra buffers param: %s",
			mmal_status_to_string(status));
		goto end;
	}
#endif

	/* copy new format */
	mmal_format_full_copy(ctx->output->format, ctx->output_format);
	if ((status = mmal_port_format_commit(ctx->output)) != MMAL_SUCCESS) {
		LOG_VPRINT_ERROR("Could not commit format change: %s",
			mmal_status_to_string(status));
		goto end;
	}

	/* setup params */
	ctx->output->buffer_num = ctx->output->buffer_num_recommended + N_EXTRA_BUFFERS;
	ctx->output->buffer_size = ctx->output->buffer_size_recommended;

	/* re-enable port */
	if ((status = mmal_port_enable(ctx->output,
		avbox_mmal_output_port_cb)) != MMAL_SUCCESS) {
		LOG_VPRINT_ERROR("Could not re-enable output port: %s",
			mmal_status_to_string(status));
		goto end;
	}

	/* create the output pool if necessary */
	if (ctx->output_pool == NULL) {
		if ((ctx->output_pool = mmal_port_pool_create(ctx->output, ctx->output->buffer_num,
			ctx->output->buffer_size)) == NULL) {
			LOG_PRINT_ERROR("Could not create output pool!");
			goto end;
		}
	}

	/* save format info */
	inst->state_info.video_res.w = ctx->output->format->es->video.width;
	inst->state_info.video_res.h = ctx->output->format->es->video.height;
	inst->state_info.time_base = AV_TIME_BASE_Q;
#ifdef ENABLE_ZERO_COPY
	inst->state_info.pix_fmt = AVBOX_PIXFMT_MMAL;
#else
	inst->state_info.pix_fmt = AVBOX_PIXFMT_YUV420P;
#endif
	ret = 0;

	/* seed output buffers */
	avbox_mmal_output_port_fill(ctx);

	DEBUG_VPRINT(LOG_MODULE, "MMAL output format changed (buffer_size=%i w=%i h=%i time_base=%i/%i pool_size=%i)",
		ctx->output->buffer_size,
		inst->state_info.video_res.w, inst->state_info.video_res.h,
		inst->state_info.time_base.num, inst->state_info.time_base.den, ctx->output->buffer_num);

end:
	mmal_format_free(ctx->output_format);
	ctx->output_format = NULL;
	return ret;
}


void *
avbox_mmal_decode(void *arg)
{
	struct avbox_player * const inst = arg;
	struct avbox_mmal_context ctx = { 0 };

	AVPacket *packet = NULL;
	AVPacket packet_copy = {0};
	MMAL_STATUS_T status;
	MMAL_COMPONENT_T *component;
	MMAL_BUFFER_HEADER_T *buffer;

	DEBUG_SET_THREAD_NAME("mmal-decode");

	ctx.inst = inst;

#if 0
	/* set the thread priority to realtime */
	struct sched_param parms;
	parms.sched_priority = sched_get_priority_max(SCHED_RR);
	if (pthread_setschedparam(pthread_self(), SCHED_RR, &parms) != 0) {
		LOG_PRINT_ERROR("Could not send main thread priority");
	}
#endif


	/* create video decoder component */
	if ((status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER,
		&component)) != MMAL_SUCCESS) {
		LOG_PRINT_ERROR("Could not create mmal component");
		component = NULL;
		goto end;
	}

	component->control->userdata = (struct MMAL_PORT_USERDATA_T*) &ctx;

	if ((status = mmal_port_enable(component->control,
		avbox_mmal_control_port_cb)) != MMAL_SUCCESS) {
		LOG_PRINT_ERROR("Could not enable MMAL port");
		goto end;
	}

	AVStream * const stream = inst->fmt_ctx->streams[inst->video_stream_index];
	ASSERT(stream != NULL);

	ctx.input = component->input[0];
	ctx.input->userdata = (struct MMAL_PORT_USERDATA_T*) &ctx;
	#if 1
	ctx.input->format->es->video.frame_rate.num = stream->r_frame_rate.num;
	ctx.input->format->es->video.frame_rate.den = stream->r_frame_rate.den;
	ctx.input->format->type = MMAL_ES_TYPE_VIDEO;
	ctx.input->format->flags = MMAL_ES_FORMAT_FLAG_FRAMED;
	#endif

	DEBUG_VPRINT(LOG_MODULE, "Input framerate %i/%i",
		stream->r_frame_rate.num, stream->r_frame_rate.den);

	switch (stream->codecpar->codec_id) {
	case AV_CODEC_ID_H264:
	{
		DEBUG_PRINT(LOG_MODULE, "Encoding: h264");
		ctx.input->format->encoding = MMAL_ENCODING_H264;
		if (stream->codecpar->extradata_size > 0) {
			if (stream->codecpar->extradata_size > 128) {
				LOG_VPRINT_ERROR("Extradata too big!: %d",
					stream->codecpar->extradata_size);
			}
			if ((status = mmal_format_extradata_alloc(ctx.input->format,
				stream->codecpar->extradata_size)) != MMAL_SUCCESS) {
				LOG_VPRINT_ERROR("Could not allocate h264 extradata: %s",
					mmal_status_to_string(status));
			} else {
				memcpy(ctx.input->format->extradata,
					stream->codecpar->extradata, stream->codecpar->extradata_size);
				ctx.input->format->extradata_size = stream->codecpar->extradata_size;
			}
		}

		/* disable error concealment */
		if ((status = mmal_port_parameter_set_boolean(ctx.input,
			MMAL_PARAMETER_VIDEO_DECODE_ERROR_CONCEALMENT, MMAL_FALSE)) != MMAL_SUCCESS) {
			LOG_VPRINT_ERROR("Could not disable error concealment: %s",
				mmal_status_to_string(status));
		}
		break;
	}
	case AV_CODEC_ID_H263:
	case AV_CODEC_ID_MPEG4:
	{
		DEBUG_PRINT(LOG_MODULE, "Encoding: MP4V");
		ctx.input->format->encoding = MMAL_ENCODING_MP4V;
		break;
	}
	case AV_CODEC_ID_MPEG1VIDEO:
	case AV_CODEC_ID_MPEG2VIDEO:
	{
		DEBUG_PRINT(LOG_MODULE, "Encoding: MP2V");
		ctx.input->format->encoding = MMAL_ENCODING_MP2V;
		break;
	}
	case AV_CODEC_ID_VP6:
	case AV_CODEC_ID_VP6F:
	case AV_CODEC_ID_VP6A:
	{
		DEBUG_PRINT(LOG_MODULE, "Encoding: VP6");
		ctx.input->format->encoding = MMAL_ENCODING_VP6;
		break;
	}
	case AV_CODEC_ID_VP8:
	{
		DEBUG_PRINT(LOG_MODULE, "Encoding: VP8");
		ctx.input->format->encoding = MMAL_ENCODING_VP8;
		break;
	}
	case AV_CODEC_ID_THEORA:
	{
		DEBUG_PRINT(LOG_MODULE, "Encoding: THEORA");
		ctx.input->format->encoding = MMAL_ENCODING_THEORA;
		break;
	}
	case AV_CODEC_ID_MJPEG:
	case AV_CODEC_ID_MJPEGB:
	{
		DEBUG_PRINT(LOG_MODULE, "Encoding: MJPEG");
		ctx.input->format->encoding = MMAL_ENCODING_MJPEG;
		break;
	}
	case AV_CODEC_ID_VC1:
	case AV_CODEC_ID_WMV3:
	{
		DEBUG_PRINT(LOG_MODULE, "Encoding: WVC1");
		ctx.input->format->encoding = MMAL_ENCODING_WVC1;
		break;
	}
	default:
		LOG_VPRINT_ERROR("Codec %i not supported!",
			stream->codecpar->codec_id);
		goto end;
	}

	if ((status = mmal_port_parameter_set_uint32(ctx.input,
		MMAL_PARAMETER_VIDEO_INTERPOLATE_TIMESTAMPS, 0)) != MMAL_SUCCESS) {
		LOG_VPRINT_ERROR("Coudl not set INTERPOLATE_TIMESTAMPTS: %s",
			mmal_status_to_string(status));
	}

#if 0
	if ((status = mmal_port_parameter_set_boolean(ctx.input,
		MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE)) != MMAL_SUCCESS) {
		LOG_VPRINT_ERROR("Could not set input zero copy!: %s",
			mmal_status_to_string(status));
	}
#endif

	if ((status = mmal_port_format_commit(ctx.input)) != MMAL_SUCCESS) {
		LOG_PRINT_ERROR("Could not commit input port format");
	}

	ctx.input->buffer_num = ctx.input->buffer_num_recommended;
	ctx.input->buffer_size = ctx.input->buffer_size_recommended;

	if ((status = mmal_port_enable(ctx.input,
		avbox_mmal_input_port_cb)) != MMAL_SUCCESS) {
		LOG_PRINT_ERROR("Could not enable mmal input port");
	}

	ctx.output = component->output[0];
	ctx.output->userdata = (struct MMAL_PORT_USERDATA_T*) &ctx;

	if ((status = mmal_port_enable(ctx.output,
		avbox_mmal_output_port_cb)) != MMAL_SUCCESS) {
		LOG_PRINT_ERROR("Could not enable mmal output port");
		return NULL;
	}

	if ((status = mmal_component_enable(component)) != MMAL_SUCCESS) {
		LOG_PRINT_ERROR("Could not initialize mmal h264 decoder!");
		return NULL;
	}

	ctx.input_pool = mmal_pool_create(ctx.input->buffer_num, 0);
	inst->state_info.pix_fmt = AVBOX_PIXFMT_YUV420P;

	avbox_checkpoint_enable(&inst->video_decoder_checkpoint);

	DEBUG_VPRINT(LOG_MODULE, "Video (MMAL) decoder ready (input pool sz=%i)",
		ctx.input->buffer_num);

	/* signal control trhead that we're ready */
	avbox_player_sendctl(inst, AVBOX_PLAYERCTL_VIDEODEC_READY, NULL);


	for (inst->video_decoder_flushed = 1;;) {

		avbox_checkpoint_here(&inst->video_decoder_checkpoint);

		/* get the next packet */
		if (packet == NULL && (packet = avbox_queue_peek(inst->video_packets_q,
			!(inst->flushing & AVBOX_PLAYER_FLUSH_VIDEO))) == NULL) {
			if (errno == EAGAIN) {
				if (inst->video_decoder_flushed ||
					!(inst->flushing & AVBOX_PLAYER_FLUSH_VIDEO)) {
					usleep(50L * 1000L);
					continue;
				}

				ctx.flushing = 1;
				/* fall through */

			} else if (errno == ESHUTDOWN) {
				if (!inst->video_decoder_flushed) {
					ctx.flushing = 1;
					/* fall through */
				} else {
					goto end;
				}
			} else {
				LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
					strerror(errno));
				break;
			}
		}

		/* change output format if necessary */
		if (ctx.output_format != NULL) {
			if (avbox_mmal_output_format_change(inst, &ctx) < 0) {
				LOG_PRINT_ERROR("Could not change output format!");
				abort();
			}
		}

		/* if we just flushed the codec pipeline flush the
		 * buffers so we can keep using it */
		if (ctx.flushing) {
			/* wait for pipeline to flush */
			int packets, frames, transit, iter_count = 0;

			DEBUG_PRINT(LOG_MODULE, "Flushing video decoder");

			/* tell MMAL to flush the decoder buffers */
			mmal_port_flush(ctx.input);
			/* mmal_port_flush(ctx.output); */

			/* wait for the buffers to flush */
			while (1) {
				packets = ctx.input_pool->headers_num -
					mmal_queue_length(ctx.input_pool->queue);
				frames = ctx.output_pool->headers_num -
					mmal_queue_length(ctx.output_pool->queue);
				transit = ctx.in_transit;

				/* NOTE: We'll always have at least one frame in transit
				 * since the player keeps it. TODO: We don't need to count
				 * transit frame here. */
				if (packets == 0 && transit <= 1) {
					break;
				}
				if (++iter_count > 50) {
					DEBUG_VPRINT(LOG_MODULE, "packets=%i frames=%i transit=%i num=%i",
						packets, frames, transit, ctx.output_pool->headers_num);
				}
				usleep(10LL * 1000LL);
			}

			/* recreate the output pool as all the buffers become
			 * invalid after flush */
			/*
			if (ctx.output_pool != NULL) {
				mmal_pool_destroy(ctx.output_pool);
				if ((ctx.output_pool = mmal_port_pool_create(ctx.output, ctx.output->buffer_num,
					ctx.output->buffer_size)) == NULL) {
					LOG_PRINT_ERROR("Could not create output pool!");
				}
			} */

			/* reload the output buffers */
			avbox_mmal_output_port_fill(&ctx);

			/* signal that flushing has completed */
			inst->video_decoder_flushed = 1;
			ctx.flushing = 0;
			ctx.time_set = 0;

			DEBUG_PRINT(LOG_MODULE, "Video decoder flushed");
			ASSERT(packet == NULL);
		}

		/* if we got a packet send it to the decoder */
		while (packet != NULL) {

			if (packet_copy.size == 0) {
				packet_copy = *packet;
			} else {
				/* should be the same packet */
				ASSERT(packet_copy.data == (packet->data + (packet->size - packet_copy.size)));
			}

			/* get a buffer header for the packet */
			if ((buffer = mmal_queue_timedwait(ctx.input_pool->queue, 10)) != NULL) {

				mmal_buffer_header_reset(buffer);
				buffer->cmd = 0;
				buffer->pts = (packet_copy.pts == AV_NOPTS_VALUE) ? MMAL_TIME_UNKNOWN :
					av_rescale_q(packet_copy.pts, stream->time_base, AV_TIME_BASE_Q);
				buffer->dts = (packet_copy.dts == AV_NOPTS_VALUE) ? MMAL_TIME_UNKNOWN :
					av_rescale_q(packet_copy.dts, stream->time_base, AV_TIME_BASE_Q);
				buffer->alloc_size = ctx.input->buffer_size;

				if (packet_copy.size > buffer->alloc_size) {
					buffer->length = buffer->alloc_size;
				} else {
					buffer->length = packet_copy.size;
				}

				buffer->data = packet_copy.data;
				buffer->flags = (packet_copy.size == packet->size) ? MMAL_BUFFER_HEADER_FLAG_FRAME_START : 0;
				packet_copy.data += buffer->length;
				packet_copy.size -= buffer->length;

				/* so we can free it later */
				if (packet_copy.size == 0) {
					buffer->flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;

					/* dequeue the packet but don't free it. It will
					 * be done by the input callback */
					if (avbox_queue_get(inst->video_packets_q) != packet) {
						LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result: %s",
							strerror(errno));
						abort();
					}

					buffer->user_data = packet;
					packet = NULL;
				}

				/* send the packet to the decoder */
				if ((status = mmal_port_send_buffer(ctx.input, buffer)) != MMAL_SUCCESS) {
					LOG_VPRINT_ERROR("Could not send packet to mmal decoder! (in_transit=%i in_decoder=%i)",
						ctx.in_transit, ctx.in_decoder);
					abort();
				}

				ATOMIC_INC(&ctx.in_decoder);

				inst->video_decoder_flushed = 0;
			} else {
				LOG_VPRINT_ERROR("Could not get input buffer from pool (in_transit=%i in_decoder=%i)",
					ctx.in_transit, ctx.in_decoder);
			}
		}
	}

end:
	DEBUG_PRINT(LOG_MODULE, "MMAL video decoder exiting");

	avbox_checkpoint_disable(&inst->video_decoder_checkpoint);

	/* signal the video thread to exit */
	if (inst->video_frames_q != NULL) {
		avbox_queue_close(inst->video_frames_q);
	}

	/* wait until all frames in transit have been
	 * flushed */
	ctx.flushing = 1;
	while (ctx.in_transit > 0) {
		usleep(100LL);
	}

	DEBUG_PRINT(LOG_MODULE, "All frames clear!");

	if (component != NULL && component->control->is_enabled) {
		mmal_port_disable(component->control);
		if (ctx.input != NULL && ctx.input->is_enabled) {
			mmal_port_disable(ctx.input);
		}
		if (ctx.output != NULL && ctx.output->is_enabled) {
			mmal_port_disable(ctx.output);
		}
		if (ctx.input_pool != NULL) {
			mmal_pool_destroy(ctx.input_pool);
		}
		if (ctx.output_pool != NULL) {
			mmal_pool_destroy(ctx.output_pool);
		}

		/* clean output_format here */

		mmal_component_release(component);
	}

	return NULL;
}
