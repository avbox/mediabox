#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#define LOG_MODULE "torrent_in"

#include "debug.h"
#include "log.h"
#include "queue.h"
#include "math_util.h"
#include "time_util.h"
#include "avbox.h"
#include "torrent_stream.h"


#define AVBOX_DVDIO_DEFLANG	"en"



struct avbox_torrentin
{
	char *path;
	int closed;
	int blocking;
	int playing;
	int waiting;

	struct avbox_torrent *stream;
	struct avbox_object *object;
	AVIOContext *avio_ctx;
	uint8_t *avio_ctx_buffer;
	struct avbox_player *player;
	pthread_cond_t waiter;
	pthread_mutex_t lock;
};


/**
 * AVIO read_packet callback.
 */
static int
avio_read_packet(void *opaque, uint8_t *buf, int bufsz)
{
	int ret = 0, bytes_read = 0;
	struct avbox_torrentin * const inst = opaque;

	if (inst->closed) {
		return 0;
	}

	while (!inst->closed && bytes_read < bufsz) {
		while ((ret = avbox_torrent_read(inst->stream, buf + bytes_read, bufsz - bytes_read)) == -1) {
			if (errno == EAGAIN && !inst->closed) {
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_BUFFER_UPDATE, NULL);
				continue;
			}
			ret = 0;
			break;
		}
		if (ret <= 0) {
			break;
		}
		bytes_read += ret;
	}

	ASSERT(bytes_read <= bufsz);

	return bytes_read;
}


/**
 * Start playing the DVD
 */
static void
play(struct avbox_torrentin * const inst, const int skip_to_menu)
{
	(void) inst;
	(void) skip_to_menu;
}


/**
 * Check is we're currently blocking the IO thread.
 */
static int
is_blocking(struct avbox_torrentin * const inst)
{
	return inst->blocking;
}


/**
 * Returns 1 if the stream is expected to underrun (meaning
 * that the player should not handle the underrun), 0
 * otherwise.
 */
static int
underrun_expected(const struct avbox_torrentin * const inst)
{
	return 0;
}


/**
 * Returns 1 if the stream can be paused.
 */
static int
can_pause(const struct avbox_torrentin * const inst)
{
	return 1;
}


static void
buffer_state(const struct avbox_torrentin * const inst,
	int64_t * const count, int64_t * const capacity)
{
	avbox_torrent_bufferstate(inst->stream,
		count, capacity);
}


/**
 * Seek the stream
 */
static int64_t
avbox_torrentin_seek(void *ctx, int64_t pos, int flags)
{
	struct avbox_torrentin * const inst = ctx;

	if (inst->closed) {
		return -1;
	}

	flags &= ~AVSEEK_FORCE;

	if (flags & AVSEEK_FLAG_FRAME) {
		ABORT("Seek to frame not supported");
	}

	/* if AVSEEK_SIZE is set just return the size */
	if (flags & AVSEEK_SIZE) {
		const int64_t ret =  avbox_torrent_size(inst->stream);
		DEBUG_VPRINT(LOG_MODULE, "Returning %" PRIi64 " to AVSEEK_SIZE",
			ret);
		return ret;

	} else if (flags & SEEK_CUR) {
		DEBUG_VPRINT(LOG_MODULE, "Relative seek to %" PRIi64 "+%" PRIi64,
			avbox_torrent_tell(inst->stream), pos);
		avbox_torrent_seek(inst->stream,
			avbox_torrent_tell(inst->stream) + pos);

	} else if (flags & SEEK_END) {
		const int64_t ret = avbox_torrent_size(inst->stream);
		if (ret == -1) {
			DEBUG_PRINT(LOG_MODULE, "Returning -1 to SEEK_END");
			return -1;
		} else {
			DEBUG_VPRINT(LOG_MODULE, "Seeking to end of file: %" PRIi64,
				ret);
			avbox_torrent_seek(inst->stream, ret);
		}

	} else {
		DEBUG_VPRINT(LOG_MODULE, "Absolute seek to %" PRIi64 " from %" PRIi64,
			pos, avbox_torrent_tell(inst->stream));
		avbox_torrent_seek(inst->stream, pos);
	}
	const int64_t ret = avbox_torrent_tell(inst->stream);
	DEBUG_VPRINT(LOG_MODULE, "Returning %" PRIi64,
		ret);
	return ret;
}


/**
 * Close the DVDIO stream.
 */
static void
close_stream(struct avbox_torrentin * const inst)
{
	DEBUG_PRINT(LOG_MODULE, "Closing torrent stream");

	ASSERT(inst != NULL);
	ASSERT(inst->avio_ctx != NULL);

	if (!inst->closed) {
		if (inst->stream != NULL) {
			avbox_torrent_close(inst->stream);
		}
		inst->closed = 1;
	} else {
		DEBUG_PRINT(LOG_MODULE, "Closing closed stream!");
	}
}


static void
destroy(struct avbox_torrentin * const inst)
{
	avbox_object_destroy(inst->object);
}


static int
control(void * context, struct avbox_message *msg)
{
	struct avbox_torrentin * const inst = context;
	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_STREAM_READY:
	{
		struct avbox_syncarg arg;
		avbox_syncarg_init(&arg, (void*) avbox_torrent_name(inst->stream));
		if (avbox_player_sendctl(inst->player,
			AVBOX_PLAYERCTL_SET_TITLE, &arg) == -1) {
			LOG_VPRINT_ERROR("Could not send SET_TITLE message: %s",
				strerror(errno));
		} else {
			avbox_syncarg_wait(&arg);
		}
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		if (!inst->closed) {
			close_stream(inst);
		}
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		if (inst->avio_ctx) {
			av_free(inst->avio_ctx);
		}
		free(inst);
		return AVBOX_DISPATCH_OK;
	}
	default:
		ABORT("Invalid message received");
	}
}


/**
 * Opens a DVD device for reading.
 */
INTERNAL struct avbox_player_stream *
avbox_torrentin_open(const char * const path, struct avbox_player * const player,
	struct avbox_player_stream * const stream)
{
	struct avbox_torrentin *inst;
	struct avbox_player_stream *ret = NULL;
	const size_t avio_ctx_bufsz = 8192;

	DEBUG_VPRINT(LOG_MODULE, "Opening torrent stream: %s", path);

	ASSERT(path != NULL);

	/* clear the function table */
	memset(stream, 0, sizeof(struct avbox_player_stream));

	if ((inst = malloc(sizeof(struct avbox_torrentin))) == NULL) {
		return NULL;
	} else {
		memset(inst, 0, sizeof(struct avbox_torrentin));
	}

	if ((inst->path = strdup(path)) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}

	/* initialize avio context */
	if ((inst->avio_ctx_buffer = av_malloc(avio_ctx_bufsz)) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}
	if ((inst->avio_ctx = avio_alloc_context(inst->avio_ctx_buffer,
		avio_ctx_bufsz, 0, inst, avio_read_packet, NULL,
		avbox_torrentin_seek)) == NULL) {
		goto end;
	}

	/* create object to receive notifications */
	if ((inst->object = avbox_object_new(&control, inst)) == NULL) {
		LOG_VPRINT_ERROR("Could not create object: %s",
			strerror(errno));
		goto end;
	}

	inst->player = player;

	/* open the torrent stream */
	if ((inst->stream = avbox_torrent_open(path, NULL,
		AVBOX_TORRENTFLAGS_STREAM, inst->object)) == NULL) {
		LOG_VPRINT_ERROR("Could not open torrent stream '%s': %s",
			path, strerror(errno));
		goto end;
	}


	/* fill the funtion table */
	stream->self = inst;
	stream->avio = inst->avio_ctx;
	stream->manages_position = 0;
	stream->buffer_state = (void*) buffer_state;
	stream->play = (void*) &play;
	stream->close = (void*) &close_stream;
	stream->destroy = (void*) &destroy;
	stream->underrun_expected = (void*) &underrun_expected;
	stream->can_pause = (void*) &can_pause;
	stream->is_blocking = (void*) &is_blocking;
	ret = stream;

end:
	if (ret == NULL && inst != NULL) {
		if (inst->object != NULL) {
			avbox_object_destroy(inst->object);
		} else { 
			if (inst->avio_ctx != NULL) {
				av_free(inst->avio_ctx);
			} else if (inst->avio_ctx_buffer != NULL) {
				av_free(inst->avio_ctx);
			}
			if (inst->stream) {
				avbox_torrent_close(inst->stream);
			}
			if (inst->path != NULL) {
				free(inst->path);
			}
			free(inst);
		}
	}

	return ret;
}
