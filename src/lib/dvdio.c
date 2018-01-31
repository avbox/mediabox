#include <limits.h>
#include <dvdnav/dvdnav.h>
#include <dvdread/dvd_reader.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <dvdnav/dvdnav.h>

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
	int still_frame;
	int have_input;

	int8_t active_stream;
	uint16_t active_stream_fmt;
	int active_stream_ch;

	uint8_t *buf;
	uint8_t mem[DVD_VIDEO_LB_LEN];
	int blen;

	dvdnav_t *dvdnav;
	AVIOContext *avio_ctx;
	uint8_t *avio_ctx_buffer;
	struct avbox_player *player;
	struct avbox_rect highlight;
	struct avbox_object *object;
	pthread_cond_t waiter;
	pthread_mutex_t lock;
};


/**
 * Process menu input. This is temporary until we get
 * subpicture support.
 */
static void
avbox_dvdio_process_menus(struct avbox_dvdio * const inst)
{
	pci_t *pci;
	int32_t btnid;

	pci = dvdnav_get_current_nav_pci(inst->dvdnav);
	dvdnav_get_current_nav_dsi(inst->dvdnav);
	dvdnav_get_current_highlight(inst->dvdnav, &btnid);

	if (pci->hli.hl_gi.btn_ns > (btnid - 1)) {
		btni_t *btn = &(pci->hli.btnit[btnid - 1]);
		if (inst->highlight.x != btn->x_start ||
			inst->highlight.y != btn->y_end ||
			inst->highlight.w != (btn->x_end - btn->x_start)) {
			inst->highlight.x = btn->x_start;
			inst->highlight.y = btn->y_end;
			inst->highlight.w = btn->x_end - btn->x_start;
			inst->highlight.h = 5;
			avbox_player_sendctl(inst->player,
				AVBOX_PLAYERCTL_UPDATE, NULL);
		}
	} else {
		if (inst->highlight.x != 0 || inst->highlight.y != 0) {
			if (inst->highlight.x || inst->highlight.y) {
				inst->highlight.x = inst->highlight.y = 0;
				avbox_player_sendctl(inst->player,
					AVBOX_PLAYERCTL_UPDATE, NULL);
			}
		}
	}
}


static int
avbox_dvdio_get_stream_id(struct avbox_dvdio * const inst, int8_t active_stream)
{
	const uint16_t format =
		dvdnav_audio_stream_format(inst->dvdnav, active_stream);
	switch (format) {
	case DVD_AUDIO_FORMAT_DTS:       return active_stream | 0x88;
	case DVD_AUDIO_FORMAT_AC3:       return active_stream | 0x80;
	case DVD_AUDIO_FORMAT_LPCM:      return active_stream | 0xa0;
	case DVD_AUDIO_FORMAT_MPEG2_EXT: return active_stream | 0xc0;
	case DVD_AUDIO_FORMAT_SDDS:      return -1;
	default:                         return -1;
	}
	return -1;
}


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
			break;
		}
		case DVDNAV_STOP:
		{
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

				/* flush */
				inst->waiting = 1;
				avbox_syncarg_init(&arg, NULL);
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_FLUSH, &arg);
				avbox_syncarg_wait(&arg);
				dvdnav_wait_skip(inst->dvdnav);
				inst->waiting = 0;
			}
			break;
		}
		case DVDNAV_STILL_FRAME:
		{
			/* if we get this while on a still frame it means it was
			 * an indefinite time still frame */
			if (inst->still_frame) {
				struct timespec tv;
				tv.tv_sec = 0;
				tv.tv_nsec = 100L * 1000L * 1000L;
				delay2abstime(&tv);
				pthread_cond_timedwait(&inst->waiter, &inst->lock, &tv);
				break;
			}

			if (!inst->playing) {
				dvdnav_still_skip(inst->dvdnav);
			} else {
				struct avbox_syncarg arg;
				dvdnav_still_event_t * const e = (dvdnav_still_event_t*) inst->buf;

				DEBUG_PRINT(LOG_MODULE, "DVDNAV_STILL_FRAME");

				inst->still_frame = 1;
				avbox_syncarg_init(&arg, (void*) ((intptr_t)e->length));
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_STILL_FRAME, &arg);

				/* if this is an indefinite time still frame then we
				 * must wait for the player to finish waiting before
				 * continuing */
				if (e->length < 0xFF) {
					avbox_syncarg_wait(&arg);
					dvdnav_still_skip(inst->dvdnav);
					inst->still_frame = 0;
				}
			}
			break;
		}
		case DVDNAV_VTS_CHANGE:
		{
			if (inst->playing) {
				struct avbox_syncarg arg;
				const char *title;
				char * m_title;
				int32_t current_title, current_part;
				uint64_t *part_times = NULL, duration = 0;
				int64_t s_duration;
				uint32_t res_x, res_y;

				DEBUG_PRINT(LOG_MODULE, "DVDNAV_VTS_CHANGE");

				inst->waiting = 1;

				/* flush player */
				avbox_syncarg_init(&arg, NULL);
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_FLUSH, &arg);
				avbox_syncarg_wait(&arg);

				if (dvdnav_get_title_string(inst->dvdnav, &title) != DVDNAV_STATUS_OK) {
					m_title = strdup("Unknown");
				} else {
					m_title = strdup(title);
				}

				if (dvdnav_current_title_info(inst->dvdnav, &current_title, &current_part) != DVDNAV_STATUS_OK) {
					LOG_VPRINT_ERROR("Could not get DVD title info: %s",
						dvdnav_err_to_string(inst->dvdnav));
					s_duration = 0;
				} else {

					(void) dvdnav_describe_title_chapters(inst->dvdnav, current_title,
						&part_times, &duration);
					if (part_times != NULL) {
						free(part_times);
						s_duration = (duration / (90L * 1000L)) * 1000L * 1000L;
					}
				}

				if (dvdnav_get_video_resolution(inst->dvdnav, &res_x, &res_y) != 0) {
					LOG_PRINT_ERROR("Could not get VTS resolution!");
				} else {
					DEBUG_VPRINT(LOG_MODULE, "Video resolution: %dx%d",
						res_x, res_y);
				}

				/* replace underscores with spaces */
				if (m_title != NULL) {
					char *p_title = m_title;
					while (*p_title != '\0') {
						if (*p_title == '_') {
							*p_title = ' ';
						}
						p_title++;
					}
					title = m_title;
				}

				/* set title */
				avbox_syncarg_init(&arg, (void*) title);
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_SET_TITLE, &arg);
				avbox_syncarg_wait(&arg);

				/* set duration */
				avbox_syncarg_init(&arg, &s_duration);
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_SET_DURATION, &arg);
				avbox_syncarg_wait(&arg);

				/* cleanup */
				if (m_title != NULL) {
					free(m_title);
				}

				inst->waiting = 0;
			}
			break;
		}
		case DVDNAV_CELL_CHANGE:
		{
			if (inst->playing) {
				int32_t tt, ptt;
				uint32_t pos, len;
				DEBUG_PRINT(LOG_MODULE, "DVDNAV_CELL_CHANGE");
				dvdnav_current_title_info(inst->dvdnav, &tt, &ptt);
				dvdnav_get_position(inst->dvdnav, &pos, &len);
				DEBUG_VPRINT(LOG_MODULE, "Cell change: Title %d, Chapter %d", tt, ptt);
				DEBUG_VPRINT(LOG_MODULE, "At pos %d/%d", pos, len);
			}
			break;
		}
		case DVDNAV_AUDIO_STREAM_CHANGE:
		{
			if (!inst->playing) {
				break;
			}

			dvdnav_audio_stream_change_event_t * const e =
				(dvdnav_audio_stream_change_event_t*) inst->buf;

			DEBUG_VPRINT(LOG_MODULE, "DVDNAV_AUDIO_STREAM_CHANGE (phys=%i|log=%i)",
				e->physical, e->logical);

			ASSERT(inst != NULL);

			(void) e;

			const int8_t active_stream =
				dvdnav_get_active_audio_stream(inst->dvdnav);
			const int active_stream_ch =
				dvdnav_audio_stream_channels(inst->dvdnav, active_stream);
			const uint16_t active_stream_fmt =
				dvdnav_audio_stream_format(inst->dvdnav, active_stream);

			if (inst->active_stream != active_stream ||
				inst->active_stream_ch != active_stream_ch ||
				inst->active_stream_fmt != active_stream_fmt) {

				struct avbox_syncarg arg;
				int stream_id = avbox_dvdio_get_stream_id(inst, active_stream);

				/* flush player */;
				avbox_syncarg_init(&arg, NULL);
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_FLUSH, &arg);
				avbox_syncarg_wait(&arg);

				/* change the stream */
				DEBUG_VPRINT(LOG_MODULE, "Switching to track id: %d", stream_id);
				avbox_syncarg_init(&arg, &stream_id);
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_CHANGE_AUDIO_TRACK, &arg);
				avbox_syncarg_wait(&arg);

				/* remember the stream */
				inst->active_stream = active_stream;
				inst->active_stream_ch = active_stream_ch;
				inst->active_stream_fmt = active_stream_fmt;
			}

			break;
		}
		case DVDNAV_SPU_CLUT_CHANGE:
		{
			if (inst->playing) {
				DEBUG_PRINT(LOG_MODULE, "DVDNAV_SPU_CLUT_CHANGE");
			}
			break;
		}
		case DVDNAV_SPU_STREAM_CHANGE:
		{
			if (inst->playing) {
				DEBUG_PRINT(LOG_MODULE, "DVDNAV_SPU_STREAM_CHANGE");
			}
			break;
		}
		case DVDNAV_HIGHLIGHT:
		{
			if (inst->playing) {
				dvdnav_highlight_event_t * const event = (dvdnav_highlight_event_t*) inst->buf;
				DEBUG_VPRINT(LOG_MODULE, "Hightlight button: %i",
					event->buttonN);
				(void) event;
			}
			break;
		}
		case DVDNAV_NAV_PACKET:
		{
			if (inst->playing) {
				int64_t pos;
				struct avbox_syncarg arg;

				avbox_dvdio_process_menus(inst);
				pos = (dvdnav_get_current_time(inst->dvdnav) / (90L * 1000L)) * 1000L * 1000L;

				/* update position */
				avbox_syncarg_init(&arg, &pos);
				avbox_player_sendctl(inst->player, AVBOX_PLAYERCTL_SET_POSITION, &arg);
				avbox_syncarg_wait(&arg);
			}
			break;
		}
		default:
			abort();
		}
		if (event != DVDNAV_BLOCK_OK) {
			inst->buf = NULL;
		}
	}
end:
	pthread_mutex_unlock(&inst->lock);
	return ret;
}


static int
avbox_dvdio_control(void * context, struct avbox_message * msg)
{
	struct avbox_dvdio * const inst = context;

	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_INPUT:
	{
		struct avbox_input_message *event =
			avbox_message_payload(msg);

		pthread_mutex_lock(&inst->lock);

		switch (event->msg) {
		case MBI_EVENT_CONTEXT:
		{
			DEBUG_PRINT(LOG_MODULE, "Menu pressed. Activating.");
			dvdnav_menu_call(inst->dvdnav, DVD_MENU_Root);
			inst->still_frame = 0;
			break;
		}
		case MBI_EVENT_ENTER:
		{
			if (!dvdnav_is_domain_vts(inst->dvdnav) && !dvdnav_is_domain_fp(inst->dvdnav)) {
				DEBUG_PRINT(LOG_MODULE, "Enter pressed. Activating.");
				dvdnav_button_activate(inst->dvdnav,
					dvdnav_get_current_nav_pci(inst->dvdnav));
				inst->still_frame = 0;
			}
			break;
		}
		case MBI_EVENT_BACK:
		{
			/* if we're in a menu go up one level */
			if (!dvdnav_is_domain_vts(inst->dvdnav) && !dvdnav_is_domain_fp(inst->dvdnav)) {
				DEBUG_PRINT(LOG_MODULE, "BACK pressed. Going one level up.");
				dvdnav_go_up(inst->dvdnav);
				inst->still_frame = 0;
			}

			/* let the shell process the event */
			pthread_cond_signal(&inst->waiter);
			pthread_mutex_unlock(&inst->lock);
			return AVBOX_DISPATCH_CONTINUE;
		}
		case MBI_EVENT_ARROW_UP:
		{
			if (!dvdnav_is_domain_vts(inst->dvdnav) && !dvdnav_is_domain_fp(inst->dvdnav)) {
				dvdnav_upper_button_select(inst->dvdnav,
					dvdnav_get_current_nav_pci(inst->dvdnav));
			}
			break;
		}
		case MBI_EVENT_ARROW_DOWN:
		{
			if (!dvdnav_is_domain_vts(inst->dvdnav) && !dvdnav_is_domain_fp(inst->dvdnav)) {
				dvdnav_lower_button_select(inst->dvdnav,
					dvdnav_get_current_nav_pci(inst->dvdnav));
			}
			break;
		}
		case MBI_EVENT_ARROW_LEFT:
		{
			if (!dvdnav_is_domain_vts(inst->dvdnav) && !dvdnav_is_domain_fp(inst->dvdnav)) {
				dvdnav_left_button_select(inst->dvdnav,
					dvdnav_get_current_nav_pci(inst->dvdnav));
			}
			break;
		}
		case MBI_EVENT_ARROW_RIGHT:
		{
			if (!dvdnav_is_domain_vts(inst->dvdnav) && !dvdnav_is_domain_fp(inst->dvdnav)) {
				dvdnav_right_button_select(inst->dvdnav,
					dvdnav_get_current_nav_pci(inst->dvdnav));
			}
			break;
		}
		default:
			pthread_mutex_unlock(&inst->lock);
			return AVBOX_DISPATCH_CONTINUE;
		}

		avbox_dvdio_process_menus(inst);
		avbox_input_eventfree(event);
		pthread_cond_signal(&inst->waiter);
		pthread_mutex_unlock(&inst->lock);

		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		DEBUG_PRINT(LOG_MODULE, "Destroying DVDIO stream");
		if (!inst->closed) {
			dvdnav_close(inst->dvdnav);
		}
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		DEBUG_PRINT(LOG_MODULE, "Cleanup DVDIO stream");
		if (inst->avio_ctx) {
			av_free(inst->avio_ctx);
		}
		free(inst);
		return AVBOX_DISPATCH_OK;
	}
	default:
		abort();
	}
}


/**
 * Start playing the DVD
 */
static void
avbox_dvdio_play(struct avbox_dvdio * const inst, const int skip_to_menu)
{
	pthread_mutex_lock(&inst->lock);
	inst->playing = 1;
	inst->buf = NULL;
	if (skip_to_menu) {
		dvdnav_menu_call(inst->dvdnav, DVD_MENU_Root);
	} else {
		dvdnav_top_pg_search(inst->dvdnav);
	}
	pthread_mutex_unlock(&inst->lock);

	/* grab input */
	if (avbox_input_grab(inst->object) == -1) {
		LOG_VPRINT_ERROR("Could not grab input: %s",
			strerror(errno));
	} else {
		inst->have_input = 1;
	}
}


/**
 * Gets the coordinates of the highlighted item.
 */
static struct avbox_rect*
avbox_dvdio_highlight(struct avbox_dvdio * const inst)
{
	if (inst->highlight.x == 0 && inst->highlight.y == 0) {
		return NULL;
	}
	return &inst->highlight;
}


/**
 * Check is we're currently blocking the IO thread.
 */
static int
avbox_dvdio_is_blocking(struct avbox_dvdio * const inst)
{
	return inst->blocking;
}


/**
 * Returns 1 if the stream is expected to underrun (meaning
 * that the player should not handle the underrun), 0
 * otherwise.
 */
static int
avbox_dvdio_underrun_expected(const struct avbox_dvdio * const inst)
{
	return  1 || ((!dvdnav_is_domain_vts(inst->dvdnav) &&
		!dvdnav_is_domain_fp(inst->dvdnav)) ||
		inst->waiting || inst->still_frame);
}


/**
 * Returns 1 if the stream can be paused.
 */
static int
avbox_dvdio_can_pause(const struct avbox_dvdio * const inst)
{
	return (dvdnav_is_domain_fp(inst->dvdnav) ||
		dvdnav_is_domain_vts(inst->dvdnav));
}


/**
 * Seek the stream
 */
static void
avbox_dvdio_seek(struct avbox_dvdio * const inst, int flags, int64_t pos)
{
	int32_t current_title, current_part, next_part, n_parts;

	if (dvdnav_current_title_info(inst->dvdnav,
		&current_title, &current_part) != DVDNAV_STATUS_OK) {
		LOG_VPRINT_ERROR("Could not get DVD title info: %s",
			dvdnav_err_to_string(inst->dvdnav));
		return;
	}

	if (current_title == -1) {
		LOG_PRINT_ERROR("Cannot seek. Currently in a menu?");
		return;
	}

	if (dvdnav_get_number_of_parts(inst->dvdnav,
		current_title, &n_parts) != DVDNAV_STATUS_OK) {
		LOG_VPRINT_ERROR("Could not get number of parts in DVD title: %s",
			dvdnav_err_to_string(inst->dvdnav));
		return;
	}

	next_part = current_part + pos;

	if (next_part > (n_parts - 1)) {
		LOG_PRINT_ERROR("Cannot seek. Already at last part");
	} else if (next_part < 0) {
		LOG_PRINT_ERROR("Cannot seek before start.");
	}

	if (dvdnav_part_play(inst->dvdnav, current_title, next_part) != DVDNAV_STATUS_OK) {
		LOG_VPRINT_ERROR("Could not seek to part %i: %s",
			next_part, dvdnav_err_to_string(inst->dvdnav));
		return;
	}
}


#if 0
/**
 * Get the dvdnav stream id that matches for
 * a given AVStream id.
 */
int
avbox_dvdio_dvdnavstream(struct avbox_dvdio * const inst, int stream_id)
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
#endif


/**
 * Close the DVDIO stream.
 */
static void
avbox_dvdio_close(struct avbox_dvdio * const inst)
{
	DEBUG_PRINT(LOG_MODULE, "Closing DVDIO");

	ASSERT(inst != NULL);
	ASSERT(inst->avio_ctx != NULL);
	ASSERT(inst->dvdnav != NULL);

	if (!inst->closed) {
		if (inst->have_input) {
			avbox_input_release(inst->object);
		}

		inst->closed = 1;
	} else {
		DEBUG_PRINT(LOG_MODULE, "Closing closed stream!");
	}
}


static void
avbox_dvdio_destroy(struct avbox_dvdio * const inst)
{
	avbox_object_destroy(inst->object);
}


/**
 * Opens a DVD device for reading.
 */
INTERNAL struct avbox_player_stream *
avbox_dvdio_open(const char * const path, struct avbox_player * const player,
	struct avbox_player_stream * const stream)
{
	struct avbox_dvdio *inst;
	struct avbox_player_stream *ret = NULL;
	const size_t avio_ctx_bufsz = 8192;
	dvdnav_status_t status;

	DEBUG_VPRINT(LOG_MODULE, "Opening device: %s", path);

	ASSERT(path != NULL);

	/* clear the function table */
	memset(stream, 0, sizeof(struct avbox_player_stream));

	if ((inst = malloc(sizeof(struct avbox_dvdio))) == NULL) {
		return NULL;
	} else {
		memset(inst, 0, sizeof(struct avbox_dvdio));
	}

	if (pthread_mutex_init(&inst->lock, NULL) != 0 ||
		pthread_cond_init(&inst->waiter, NULL) != 0) {
		LOG_PRINT_ERROR("Could not initialize mutex!!!");
		goto end;
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
		avio_ctx_bufsz, 0, inst, avio_read_packet, NULL, NULL)) == NULL) {
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
	inst->still_frame = 0;
	inst->active_stream = -1;
	inst->active_stream_ch = 0;
	inst->active_stream_fmt = 0xffff;
	inst->player = player;

	/* create a dispatch object */
	if ((inst->object = avbox_object_new(avbox_dvdio_control, inst)) == NULL) {
		LOG_PRINT_ERROR("Could not create dispatch object");
		goto end;
	}

	/* fill the funtion table */
	stream->self = inst;
	stream->avio = inst->avio_ctx;
	stream->manages_position = 1;
	stream->play = (void*) &avbox_dvdio_play;
	stream->seek = (void*) &avbox_dvdio_seek;
	stream->close = (void*) &avbox_dvdio_close;
	stream->destroy = (void*) &avbox_dvdio_destroy;
	stream->underrun_expected = (void*) &avbox_dvdio_underrun_expected;
	stream->can_pause = (void*) &avbox_dvdio_can_pause;
	stream->is_blocking = (void*) &avbox_dvdio_is_blocking;
	stream->highlight = (void*) &avbox_dvdio_highlight;
	ret = stream;

end:
	if (ret == NULL && inst != NULL) {
		if (inst->avio_ctx != NULL) {
			av_free(inst->avio_ctx);
		} else if (inst->avio_ctx_buffer != NULL) {
			av_free(inst->avio_ctx);
		}
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
