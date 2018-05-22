/**
 * MediaBox - Linux based set-top firmware
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


#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <libgen.h>

#define LOG_MODULE "overlay"

#include <libavbox/avbox.h>
#include "library.h"
#include "overlay.h"


struct mbox_overlay
{
	struct avbox_window *window;
	struct avbox_window *duration_view;
	struct avbox_window *icon_window;
	struct avbox_window *bar_window;
	struct avbox_window *title_window;
	struct avbox_player *player;
	enum mbv_alignment alignment;
	int state;
	int dismiss_timer;
	int duration_timer;
	int last_state;
	int64_t duration;
	int64_t last_duration;
	int64_t last_bar_pos;
	int64_t position;
	int64_t last_position;
	char *title;
};


static void
avbox_overlay_formatpos(char *buf, size_t bufsz, int64_t position, int64_t duration)
{
	double pos = position;
	double dur = duration;
	int pos_hours, pos_mins, pos_secs;
	int dur_hours, dur_mins, dur_secs;

	pos_hours = pos / (1000.0 * 1000.0 * 60.0 * 60.0);
	pos -= pos_hours * 1000.0 * 1000.0 * 60.0 * 60.0;
	pos_mins = pos / (1000.0 * 1000.0 * 60.0);
	pos -= pos_mins * 1000.0 * 1000.0 * 60.0;
	pos_secs = pos / (1000.0 * 1000.0);

	dur_hours = dur / (1000.0 * 1000.0 * 60.0 * 60.0);
	dur -= dur_hours * 1000.0 * 1000.0 * 60.0 * 60.0;
	dur_mins = dur / (1000.0 * 1000.0 * 60.0);
	dur -= dur_mins * 1000.0 * 1000.0 * 60.0;
	dur_secs = dur / (1000.0 * 1000.0);

	snprintf(buf, bufsz, "%02d:%02d:%02d/%02d:%02d:%02d",
		(int) pos_hours, (int) pos_mins, (int) pos_secs,
		(int) dur_hours, (int) dur_mins, (int) dur_secs);
}


/**
 * Strip extension.
 */
static void
stripext(char * filename)
{
	char *tmp = filename + strlen(filename) - 1;
	while (tmp > filename) {
		if (*tmp == '.') {
			*tmp = '\0';
			break;
		}
		tmp--;
	}
}


static int
mbox_title_draw(struct avbox_window * const window, void * const ctx)
{
	cairo_t *context;
	PangoLayout *msg;
	int w, h;
	struct mbox_overlay * const inst = ctx;
	PangoFontDescription *font_desc;

	if (!avbox_window_dirty(window)) {
		return 0;
	}

	avbox_window_getcanvassize(window, &w, &h);

	avbox_window_clear(window);

	if ((context = avbox_window_cairo_begin(window)) != NULL) {

		cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);

		/* draw the title */
		if ((msg = pango_cairo_create_layout(context)) != NULL) {
			if ((font_desc = pango_font_description_from_string("Sans Bold 24px")) != NULL) {
				pango_layout_set_font_description(msg, font_desc);
				pango_layout_set_width(msg, w * PANGO_SCALE);
				pango_layout_set_height(msg, h * PANGO_SCALE);
				pango_layout_set_ellipsize(msg, PANGO_ELLIPSIZE_MIDDLE);
				pango_layout_set_alignment(msg, mbv_get_pango_alignment(inst->alignment));
				pango_layout_set_text(msg, inst->title, -1);
				pango_cairo_update_layout(context, msg);
				pango_font_description_free(font_desc);
				cairo_translate(context, 0, 0);
				pango_cairo_show_layout(context, msg);
				g_object_unref(msg);
			}
		}

		avbox_window_cairo_end(window);
	}

	avbox_window_setdirty(window, 0);

	return 0;

}


static int
mbox_bar_draw(struct avbox_window * const window, void * const ctx)
{
	int w, h;
	struct avbox_rect bar_rect;
	struct mbox_overlay * const inst = ctx;

	if (!avbox_window_dirty(window)) {
		return 0;
	}

	DEBUG_PRINT(LOG_MODULE, "Redrawing bar");

	avbox_window_getcanvassize(window, &w, &h);

	bar_rect.x = 0;
	bar_rect.y = 0;
	bar_rect.w = w;
	bar_rect.h = h;

	if (inst->duration == 0) {
		inst->duration = 1;
	}

	/* draw the bar */
	avbox_window_setbgcolor(inst->bar_window, AVBOX_COLOR(0x0000ffbf));
	avbox_window_roundrectangle(window, &bar_rect, 2, 10);
	bar_rect.w = (bar_rect.w * ((inst->position * 100.0) / (double) inst->duration)) / 100.0;
	avbox_window_setbgcolor(window, AVBOX_COLOR(0xffffffff));
	avbox_window_roundrectangle(window, &bar_rect, 2, 10);

	avbox_window_setdirty(window, 0);

	return 0;
}


static int
mbox_icon_draw(struct avbox_window * const window, void * const ctx)
{
	cairo_t *context;
	int w, h;
	struct mbox_overlay * const inst = ctx;

	if (!avbox_window_dirty(window)) {
		return 0;
	}

	avbox_window_getcanvassize(window, &w, &h);

	avbox_window_clear(window);

	if ((context = avbox_window_cairo_begin(window)) != NULL) {

		cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);

		/* draw the icon */
		switch (inst->state) {
		case MBOX_OVERLAY_STATE_READY:
		{
			cairo_move_to(context, 0, 0);
			cairo_line_to(context, 0, h);
			cairo_line_to(context, 30, h);
			cairo_line_to(context, 30, 0);
			cairo_line_to(context, 0, 0);
			cairo_fill(context);
			break;
		}
		case MBOX_OVERLAY_STATE_PLAYING:
		{
			cairo_move_to(context, 0, 0);
			cairo_line_to(context, 0, h);
			cairo_line_to(context, 30, h >> 1);
			cairo_fill(context);
			break;
		}
		case MBOX_OVERLAY_STATE_PAUSED:
		{
			cairo_move_to(context, 0, 0);
			cairo_line_to(context, 0, h);
			cairo_line_to(context, 10, h);
			cairo_line_to(context, 10, 0);
			cairo_line_to(context, 0, 0);
			cairo_fill(context);
			cairo_move_to(context, 20, 0);
			cairo_line_to(context, 20, h);
			cairo_line_to(context, 30, h);
			cairo_line_to(context, 30, 0);
			cairo_line_to(context, 20, 0);
			cairo_fill(context);
			break;
		}
		default:
			DEBUG_VPRINT(LOG_MODULE, "Invalid state: %i",
				inst->state);
			ABORT("Invalid state!");
		}

		avbox_window_cairo_end(window);
	}

	avbox_window_setdirty(window, 0);

	return 0;
}


static int
mbox_duration_draw(struct avbox_window * const window, void * const ctx)
{
	cairo_t *context;
	PangoLayout *msg;
	int w, h;
	struct mbox_overlay * const inst = ctx;
	PangoFontDescription *font_desc;

	if (!avbox_window_dirty(window)) {
		return 0;
	}

	avbox_window_getcanvassize(window, &w, &h);

	avbox_window_clear(window);

	if ((context = avbox_window_cairo_begin(window)) != NULL) {

		cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);

		/* draw the duration */
		if ((msg = pango_cairo_create_layout(context)) != NULL) {
			if ((font_desc = pango_font_description_from_string("Sans Bold 18px")) != NULL) {
				char duration[20];
				avbox_overlay_formatpos(duration, sizeof(duration), inst->position, inst->duration);
				pango_layout_set_font_description(msg, font_desc);
				pango_layout_set_width(msg, w * PANGO_SCALE);
				pango_layout_set_alignment(msg, PANGO_ALIGN_RIGHT);
				pango_layout_set_text(msg, duration, -1);
				pango_cairo_update_layout(context, msg);
				pango_font_description_free(font_desc);
				cairo_translate(context, 0, 0);
				pango_cairo_show_layout(context, msg);
				g_object_unref(msg);
			}
		}
		avbox_window_cairo_end(window);
	}

	avbox_window_setdirty(window, 0);

	return 0;
}


/**
 * Draws the overlay window.
 */
static int
mbox_overlay_draw(struct avbox_window *window, void * const ctx)
{
	struct mbox_overlay * const inst = ctx;
	struct avbox_rect rect;

	ASSERT(inst != NULL);

	if (!avbox_window_dirty(window)) {
		return 0;
	}

	avbox_window_getcanvassize(window, &rect.w, &rect.h);

	rect.x = 0;
	rect.y = 0;

	/* draw the round window */
	avbox_window_setbgcolor(inst->window, AVBOX_COLOR(0xcccccc00));
	avbox_window_clear(window);
	avbox_window_setcolor(window, AVBOX_COLOR(0xffffffff));
	avbox_window_setbgcolor(window, AVBOX_COLOR(0x0000ffbf));
	avbox_window_roundrectangle(window, &rect, 2, 5);
	avbox_window_setdirty(window, 0);
	return 0;
}


/**
 * Set the overlay state
 */
static void
mbox_overlay_setstate(struct mbox_overlay * const inst, int state)
{
	ASSERT(inst != NULL);
	ASSERT(state == MBOX_OVERLAY_STATE_READY ||
		state == MBOX_OVERLAY_STATE_PLAYING ||
		state == MBOX_OVERLAY_STATE_PAUSED);
	if (inst->state != state) {
		inst->state = state;
		avbox_window_setdirty(inst->icon_window, 1);
	}
}


/**
 * Set the title.
 */
static int
mbox_overlay_settitle(struct mbox_overlay * const inst, char * const title)
{
	char *newtitle;

	ASSERT(title != NULL);

	/* if the title is the same there's nothing to do */
	if (!strcmp(title, inst->title)) {
		return 0;
	}

	/* copy the title */
	if ((newtitle = strdup(title)) == NULL) {
		ASSERT(errno == ENOMEM);
		return -1;
	}

	/* replace old title with new one */
	free(inst->title);
	inst->title = newtitle;
	avbox_window_setdirty(inst->title_window, 1);
	return 0;
}


/**
 * Checks if a path is in the library watch list.
 */
static const char *
mbox_overlay_is_in_watchdir(const char * const path)
{
	const char **paths = mbox_library_watchdirs();

	while (*paths != NULL) {
		const size_t len = strlen(*paths);
		if (!strncmp(*paths, path, len)) {
			return path + len;
		}
		paths++;
	}

	return NULL;
}


static inline void
mbox_overlay_start_time_updates(struct mbox_overlay * const inst, const int force)
{
	if (force || inst->duration_timer == -1) {
		struct timespec tv;
		tv.tv_sec = 1;
		tv.tv_nsec = 0;
		if ((inst->duration_timer = avbox_timer_register(&tv,
			AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
			avbox_window_object(inst->window), NULL, NULL)) == -1) {
			LOG_VPRINT_ERROR("Could not register overlay timer: %s",
				strerror(errno));
			avbox_window_hide(inst->window);
		}
	}

}


/**
 * Message handler.
 */
static int
mbox_overlay_handler(void *context, struct avbox_message *msg)
{
	int msgtype;
	struct mbox_overlay * const inst = context;
	switch ((msgtype = avbox_message_id(msg))) {
	case AVBOX_MESSAGETYPE_PLAYER:
	{
		struct avbox_player_status_data * const data =
			avbox_message_payload(msg);
		if (data->last_status == MB_PLAYER_STATUS_PAUSED &&
			data->status != MB_PLAYER_STATUS_PAUSED) {
			mbox_overlay_setstate(inst, MBOX_OVERLAY_STATE_PLAYING);
			mbox_overlay_show(inst, 15);
		}

		/* if we're out of the ready state start the timer
		 * to update the duration */
		if (data->last_status == MB_PLAYER_STATUS_READY &&
			data->status != MB_PLAYER_STATUS_READY) {
			if (avbox_window_isvisible(inst->window)) {
				mbox_overlay_start_time_updates(inst, 0);
			}
		}

		switch (data->status) {
		case MB_PLAYER_STATUS_READY:
			if (avbox_window_isvisible(inst->window)) {
				avbox_window_hide(inst->window);
			}
			mbox_overlay_setstate(inst, MBOX_OVERLAY_STATE_READY);
			break;
		case MB_PLAYER_STATUS_BUFFERING:
			if (data->last_status != MB_PLAYER_STATUS_BUFFERING) {
				mbox_overlay_setstate(inst, MBOX_OVERLAY_STATE_READY);
				mbox_overlay_show(inst, 15);
			}
			/* fall through */
		case MB_PLAYER_STATUS_PLAYING:
		{
			char *title;
			if ((title = avbox_player_gettitle(inst->player)) != NULL) {
				const char *short_title;

				/* if the title is a path to a file on a watched
				 * directory format it as best as possible */
				if ((short_title = mbox_overlay_is_in_watchdir(title)) != NULL) {
					char *copy;
					if ((copy = strdup(title)) != NULL) {
						char *base = basename(copy);
						if (strncmp(base, "Episode", 7)) {
							char *basecopy;
							if ((basecopy = strdup(base)) != NULL) {
								stripext(basecopy);
								free(title);
								title = basecopy;
							} else {
								LOG_VPRINT_ERROR("Could not duplicate basename: %s",
									strerror(errno));
							}
						} else {
							char *tmp;
							if ((tmp = strdup(short_title)) != NULL) {
								stripext(tmp);
								free(title);
								title = tmp;
							} else {
								LOG_VPRINT_ERROR("Could not duplicated truncated name: %s",
									strerror(errno));
							}
						}
						free(copy);
					} else {
						LOG_VPRINT_ERROR("Could not allocate title copy: %s",
							strerror(errno));
					}
				}
				mbox_overlay_settitle(inst, title);
				free(title);
			} else {
				mbox_overlay_settitle(inst, "Unknown");
			}
			mbox_overlay_setstate(inst, MBOX_OVERLAY_STATE_PLAYING);
			mbox_overlay_show(inst, 15);
			break;
		}
		case MB_PLAYER_STATUS_PAUSED:
			mbox_overlay_setstate(inst, MBOX_OVERLAY_STATE_PAUSED);
			mbox_overlay_show(inst, 15);
			break;
		}
		return AVBOX_DISPATCH_CONTINUE;
	}
	case AVBOX_MESSAGETYPE_TIMER:
	{
		struct avbox_timer_data * const data =
			avbox_message_payload(msg);
		ASSERT(data != NULL);
		if (data->id == inst->dismiss_timer) {
			if (avbox_window_isvisible(inst->window)) {
				avbox_window_hide(inst->window);
			}
			inst->dismiss_timer = -1;
		} else if (data->id == inst->duration_timer) {
			avbox_player_gettime(inst->player, &inst->position);
			avbox_player_getduration(inst->player, &inst->duration);
			if (inst->position != inst->last_position || inst->duration != inst->last_duration) {
				/* only redraw the bar every 5 seconds */
				if (labs(inst->position - inst->last_bar_pos) >= 5 * 1000LL * 1000LL) {
					avbox_window_setdirty(inst->bar_window, 1);
					inst->last_bar_pos = inst->position;
				}
				inst->last_position = inst->position;
				inst->last_duration = inst->duration;
				avbox_window_setdirty(inst->duration_view, 1);
			}
			if (avbox_window_isvisible(inst->window) && inst->state != MB_PLAYER_STATUS_READY) {
				mbox_overlay_start_time_updates(inst, 1);
			} else {
				inst->duration_timer = -1;
			}
		} else {
			DEBUG_VPRINT("overlay", "Message for unknown timer %i received!",
				data->id);
		}
		avbox_timers_releasepayload(data);
		break;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
		if (inst->dismiss_timer != -1) {
			avbox_timer_cancel(inst->dismiss_timer);
			inst->dismiss_timer = -1;
		}
		if (inst->duration_timer != -1) {
			avbox_timer_cancel(inst->duration_timer);
			inst->duration_timer = -1;
		}
		if (inst->player != NULL) {
			if (avbox_player_unsubscribe(inst->player,
				avbox_window_object(inst->window)) == -1) {
				LOG_VPRINT_ERROR("Could not unsubscribe from player events: %s",
					strerror(errno));
			}
		}
		free(inst->title);
		break;
	case AVBOX_MESSAGETYPE_CLEANUP:
		free(inst);
		break;
	default:
		DEBUG_VPRINT("overlay", "Invalid message type: %d",
			msgtype);
		abort();
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Show the overlay.
 */
void
mbox_overlay_show(struct mbox_overlay * const inst, int secs)
{
	struct timespec tv;

	/* if the overlay is visible hide it and cancel
	 * the timer */
	if (avbox_window_isvisible(inst->window)) {
		ASSERT(inst->dismiss_timer != -1);
		avbox_timer_cancel(inst->dismiss_timer);
		inst->dismiss_timer = -1;
	} else {
		/* show the overlay */
		avbox_window_show(inst->window);
	}

	/* if we're out of the ready state and the duration timer
	 * is not running then start it */
	if (inst->state != MB_PLAYER_STATUS_READY) {
		mbox_overlay_start_time_updates(inst, 0);
	}

	/* start the timer to dismiss it */
	tv.tv_sec = secs;
	tv.tv_nsec = 0;
	if ((inst->dismiss_timer = avbox_timer_register(&tv,
		AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
		avbox_window_object(inst->window), NULL, NULL)) == -1) {
		LOG_VPRINT_ERROR("Could not register overlay timer: %s",
			strerror(errno));
		avbox_window_hide(inst->window);
	}
}



/**
 * Create an overlay instance.
 */
struct mbox_overlay *
mbox_overlay_new(struct avbox_player *player)
{
	int w, h;
	struct mbox_overlay *inst;

	/* get the display dimensions */
	avbox_window_getcanvassize(
		avbox_video_getrootwindow(0), &w, &h);

	/* allocate memory for overlay */
	if ((inst = malloc(sizeof(struct mbox_overlay))) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}

	/* create the overlay window */
	if ((inst->window = avbox_window_new(NULL, "overlay",
		AVBOX_WNDFLAGS_ALPHABLEND, 80, 70, w - 160, 80,
		mbox_overlay_handler, mbox_overlay_draw, inst)) == NULL) {
		free(inst);
		return NULL;
	}

	if ((inst->title_window = avbox_window_new(inst->window, "title",
		AVBOX_WNDFLAGS_SUBWINDOW, 50, 10, w - 160 - 60, 25,
		NULL, mbox_title_draw, inst)) == NULL) {
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	if ((inst->duration_view = avbox_window_new(inst->window, "duration",
		AVBOX_WNDFLAGS_SUBWINDOW, w - 160 - 250, 80 - 25, 240, 20,
		NULL, mbox_duration_draw, inst)) == NULL) {
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	if ((inst->icon_window = avbox_window_new(inst->window, "icon",
		AVBOX_WNDFLAGS_SUBWINDOW, 10, 10, 30, 65,
		NULL, mbox_icon_draw, inst)) == NULL) {
		avbox_window_destroy(inst->duration_view);
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	if ((inst->bar_window = avbox_window_new(inst->window, "bar",
		AVBOX_WNDFLAGS_SUBWINDOW, 50, 40, w - 160 - 60, 10,
		NULL, mbox_bar_draw, inst)) == NULL) {
		avbox_window_destroy(inst->icon_window);
		avbox_window_destroy(inst->duration_view);
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	/* subscribe to player events */
	if (avbox_player_subscribe(player, avbox_window_object(inst->window)) == -1) {
		LOG_VPRINT_ERROR("Could not subscribe to player events: %s",
			strerror(errno));
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	avbox_window_setcolor(inst->duration_view, AVBOX_COLOR(0xffffffff));
	avbox_window_setbgcolor(inst->duration_view, AVBOX_COLOR(0x0000ffbf));
	avbox_window_setcolor(inst->icon_window, AVBOX_COLOR(0xffffffff));
	avbox_window_setbgcolor(inst->icon_window, AVBOX_COLOR(0x0000ffbf));
	avbox_window_setcolor(inst->bar_window, AVBOX_COLOR(0xffffffff));
	avbox_window_setbgcolor(inst->bar_window, AVBOX_COLOR(0x0000ffbf));
	avbox_window_setcolor(inst->title_window, AVBOX_COLOR(0xffffffff));
	avbox_window_setbgcolor(inst->title_window, AVBOX_COLOR(0x0000ffbf));

	inst->title = strdup("NONE");
	inst->alignment = MBV_ALIGN_LEFT;
	inst->dismiss_timer = -1;
	inst->duration_timer = -1;
	inst->player = player;
	inst->duration = 0;
	inst->position = 0;
	inst->state = MBOX_OVERLAY_STATE_READY;
	inst->last_state = -1;
	inst->last_bar_pos = -1;
	inst->last_duration = -1;
	inst->last_position = -1;

	return inst;
}


/**
 * Get the underlying window.
 */
struct avbox_window *
mbox_overlay_window(const struct mbox_overlay * const inst)
{
	return inst->window;
}
