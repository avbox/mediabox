#include <limits.h>
#include <dvdnav/dvdnav.h>
#include <dvdread/dvd_reader.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#define LOG_MODULE "dvdio"

#include "debug.h"
#include "log.h"
#include "dvdio.h"
#include "queue.h"
#include "math_util.h"
#include "time_util.h"
#include "avbox.h"


#define AVBOX_DVDIO_DEFLANG	"en"



struct avbox_dvdio
{
	char *path;
	int closed;
	int blocking;
	int playing;
	int waiting;

	uint8_t *buf;
	uint8_t mem[DVD_VIDEO_LB_LEN];
	int blen;

	dvdnav_t *dvdnav;
	AVIOContext *avio_ctx;
	void *callback_context;
	avbox_dvdio_dvdnavcb callback;
	uint8_t *avio_ctx_buffer;
	struct avbox_player *player;
	pthread_mutex_t lock;
};


/**
 * AVIO read_packet callback.
 */
static int
avio_read_packet(void *opaque, uint8_t *buf, int bufsz)
{
	int ret = 0;
	struct avbox_dvdio * const inst = opaque;
	int result, event, len;

	pthread_mutex_lock(&inst->lock);
	inst->blocking = 1;

	while (1) {

		if (inst->closed) {
			inst->blocking = 0;
			ret = 0;
			goto end;
		}

		/* get the next block */
		if (inst->buf == NULL) {
			if ((result = dvdnav_get_next_block(
				inst->dvdnav, inst->mem, &event, &len)) == DVDNAV_STATUS_ERR) {
				LOG_VPRINT_ERROR("Could not get next block: %s",
					dvdnav_err_to_string(inst->dvdnav));
				inst->blocking = 0;
				ret = 0;
				goto end;
			} else {
				inst->buf = inst->mem;
				inst->blen = len;
			}
		} else {
			if (inst->blen <= bufsz) {
				memcpy(buf, inst->buf, inst->blen);
				inst->buf = NULL;
				inst->blocking = 0;
				ret = inst->blen;
				goto end;
			} else {
				memcpy(buf, inst->buf, bufsz);
				inst->blen -= bufsz;
				inst->buf += bufsz;
				inst->blocking = 0;
				ret = bufsz;
				goto end;
			}
		}

		switch (event) {
		case DVDNAV_BLOCK_OK:
		{
			if (inst->blen <= bufsz) {
				memcpy(buf, inst->buf, inst->blen);
				inst->buf = NULL;
				inst->blocking = 0;
				ret = inst->blen;
				goto end;
			} else {
				memcpy(buf, inst->buf, bufsz);
				inst->blen -= bufsz;
				inst->buf += bufsz;
				inst->blocking = 0;
				ret = bufsz;
				goto end;
			}
		}
		case DVDNAV_NOP:
		{
			inst->buf = NULL;
			break;
		}
		case DVDNAV_STOP:
		{
			inst->buf = NULL;
			inst->closed = 1;
			break;
		}
		case DVDNAV_HOP_CHANNEL:
			DEBUG_PRINT(LOG_MODULE, "DVDNAV_HOP_CHANNEL");
		case DVDNAV_WAIT:
		{
			if (!inst->playing) {
				dvdnav_wait_skip(inst->dvdnav);
			} else {
				struct avbox_syncarg arg;

				DEBUG_PRINT(LOG_MODULE, "DVDNAV_WAIT");

				avbox_syncarg_init(&arg, NULL);
				inst->waiting = 1;
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_FLUSH, &arg);
				avbox_syncarg_wait(&arg);
				dvdnav_wait_skip(inst->dvdnav);
				inst->waiting = 0;
			}
			break;
		}
		case DVDNAV_STILL_FRAME:
		{
			if (!inst->playing) {
				dvdnav_still_skip(inst->dvdnav);
			} else {
				struct avbox_syncarg arg;
				dvdnav_still_event_t * const e = (dvdnav_still_event_t*) inst->buf;

				DEBUG_PRINT(LOG_MODULE, "DVDNAV_STILL_FRAME");

				avbox_syncarg_init(&arg, (void*) ((intptr_t)e->length));
				inst->waiting = 1;
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_STILL_FRAME, &arg);
				avbox_syncarg_wait(&arg);
				dvdnav_still_skip(inst->dvdnav);
				inst->waiting = 0;
			}
			break;
		}
		default:
			if (!inst->playing) {
				switch (event) {
				case DVDNAV_STILL_FRAME:
				{
					dvdnav_still_skip(inst->dvdnav);
					break;
				}
				default:
					break;
				}
			} else {
				if (inst->callback != NULL) {
					inst->callback(inst->callback_context, event, inst->buf);
				}
			}
			inst->buf = NULL;
			break;
		}
	}
end:
	pthread_mutex_unlock(&inst->lock);
	return ret;
}


/**
 * Start playing the DVD
 */
void
avbox_dvdio_play(struct avbox_dvdio * const inst)
{
	pthread_mutex_lock(&inst->lock);
	inst->playing = 1;
	inst->buf = NULL;
	dvdnav_top_pg_search(inst->dvdnav);
	/* dvdnav_menu_call(inst->dvdnav, DVD_MENU_Root); */
	pthread_mutex_unlock(&inst->lock);
}


/**
 * Get an AVIOContext for this opened DVD.
 */
AVIOContext *
avbox_dvdio_avio(struct avbox_dvdio * const inst)
{
	return inst->avio_ctx;
}


/**
 * Gets the dvdnav instance.
 */
dvdnav_t *
avbox_dvdio_dvdnav(struct avbox_dvdio * const inst)
{
	return inst->dvdnav;
}


/**
 * Check is we're currently blocking the IO thread.
 */
int
avbox_dvdio_isblocking(struct avbox_dvdio * const inst)
{
	return inst->blocking;
}


/**
 * Returns 1 if the stream is expected to underrun (meaning
 * that the player should not handle the underrun), 0
 * otherwise.
 */
int
avbox_dvdio_underrunok(const struct avbox_dvdio * const inst)
{
	return (!dvdnav_is_domain_vts(inst->dvdnav) &&
		!dvdnav_is_domain_fp(inst->dvdnav)) ||
		inst->waiting;
}


/**
 * Get the dvdnav stream id that matches for
 * a given AVStream id.
 */
int
avbox_dvdio_dvdnavstream(int stream_id)
{
	/* find the stream id */
	if ((stream_id & 0xf8) == 0x88) { /* dts */
		stream_id &= 0x07;
	} else if ((stream_id & 0xf0) == 0x80) { /* a52 */
		stream_id &= 0xf;
	} else if ((stream_id & 0xf0) == 0xa0) { /* lpcm */
		stream_id &= 0x1f;
	} else if ((stream_id & 0xe0) == 0xc0) { /* mpga */
		stream_id &= 0x1f;
	} else /*if ((i_id & 0x80) == 0x80)*/ {
		/* i_id &= 0x07; */
		LOG_VPRINT_ERROR("Could not map stream: %i!",
			stream_id);
		stream_id = -1;
	}

	return stream_id;
}


static int
avbox_dvdio_initavio(struct avbox_dvdio * const inst)
{
	int ret = -1;
	const size_t avio_ctx_bufsz = 8192;
	/* initialize avio context */
	if ((inst->avio_ctx_buffer = av_malloc(avio_ctx_bufsz)) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}
	if ((inst->avio_ctx = avio_alloc_context(inst->avio_ctx_buffer,
		avio_ctx_bufsz, 0, inst, avio_read_packet, NULL, NULL)) == NULL) {
		goto end;
	}
	ret = 0;
end:
	if (ret == -1) {
		if (inst->avio_ctx != NULL) {
			av_free(inst->avio_ctx);
		} else if (inst->avio_ctx_buffer != NULL) {
			av_free(inst->avio_ctx);
		}
	}

	return ret;
}

/**
 * Opens a DVD device for reading.
 */
struct avbox_dvdio *
avbox_dvdio_open(const char * const path, struct avbox_player * const player,
	avbox_dvdio_dvdnavcb callback, void *callback_context)
{
	struct avbox_dvdio *inst;
	struct avbox_dvdio *ret = NULL;
	dvdnav_status_t status;

	DEBUG_VPRINT(LOG_MODULE, "Opening device: %s", path);

	ASSERT(path != NULL);

	if ((inst = malloc(sizeof(struct avbox_dvdio))) == NULL) {
		return NULL;
	} else {
		memset(inst, 0, sizeof(struct avbox_dvdio));
	}

	if (pthread_mutex_init(&inst->lock, NULL) != 0) {
		LOG_PRINT_ERROR("Could not initialize mutex!!!");
		goto end;
	}

	if ((inst->path = strdup(path)) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}

	/* initialize avio context */
	if (avbox_dvdio_initavio(inst) == -1) {
		goto end;
	}

	/* open the dvdnav stream */
	status = dvdnav_open(&inst->dvdnav, path);
	if (status != DVDNAV_STATUS_OK) {
		goto end;
	}

	/* set default language */
	if (dvdnav_menu_language_select(inst->dvdnav, "en") != DVDNAV_STATUS_OK ||
		dvdnav_audio_language_select(inst->dvdnav, "en") != DVDNAV_STATUS_OK ||
		dvdnav_spu_language_select(inst->dvdnav, "en") != DVDNAV_STATUS_OK) {
		goto end;
	}

	/* set PGC positioning flag */
	if (dvdnav_set_PGC_positioning_flag(inst->dvdnav, 1) != DVDNAV_STATUS_OK) {
		goto end;
	}

	inst->closed = 0;
	inst->blen = 0;
	inst->buf = NULL;
	inst->blocking = 0;
	inst->playing = 0;
	inst->waiting = 0;
	inst->player = player;
	inst->callback = callback;
	inst->callback_context = callback_context;
	ret = inst;

end:
	if (ret == NULL && inst != NULL) {
		if (inst->dvdnav) {
			dvdnav_close(inst->dvdnav);
		}
		if (inst->path != NULL) {
			free(inst->path);
		}
		free(inst);
	}

	return ret;
}


/**
 * Close the DVD.
 */
void
avbox_dvdio_close(struct avbox_dvdio * const inst)
{
	DEBUG_PRINT(LOG_MODULE, "Closing DVDIO");

	ASSERT(inst != NULL);
	ASSERT(inst->avio_ctx != NULL);
	ASSERT(inst->dvdnav != NULL);
	inst->closed = 1;
}


/**
 * Reopen a closed dvdio stream.
 */
int
avbox_dvdio_reopen(struct avbox_dvdio * const inst)
{
	DEBUG_PRINT(LOG_MODULE, "Re-opening DVDIO");

	ASSERT(inst != NULL);
	ASSERT(inst->avio_ctx != NULL);
	ASSERT(inst->dvdnav != NULL);
	inst->closed = 0;
	return 0;
}


/**
 * Free DVDIO resources
 */
void
avbox_dvdio_destroy(struct avbox_dvdio * const inst)
{
	DEBUG_PRINT(LOG_MODULE, "Destroying DVDIO");
	dvdnav_close(inst->dvdnav);
	if (inst->avio_ctx) {
		av_free(inst->avio_ctx);
	}
	free(inst);
}
