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

#include "lib/avbox.h"
#include "overlay.h"


struct mbox_overlay
{
	struct avbox_window *window;
	struct avbox_player *player;
	enum mbv_alignment alignment;
	int state;
	int dirty;
	int dismiss_timer;
	int duration_timer;
	int64_t duration;
	int64_t position;
	char *title;
};


static void
avbox_overlay_formatpos(char *buf, int64_t position, int64_t duration)
{
	int pos_hours, pos_mins, pos_secs;
	int dur_hours, dur_mins, dur_secs;
	pos_hours = position / (1000L * 1000L * 60 * 60);
	position -= pos_hours * 1000L * 1000L * 60 * 60;
	pos_mins = position / (1000L * 1000L * 60);
	position -= pos_mins * 1000L * 1000L * 60;
	pos_secs = position / (1000L * 1000L);
	dur_hours = duration / (1000L * 1000L * 60 * 60);
	duration -= dur_hours * 1000L * 1000L * 60 * 60;
	dur_mins = duration / (1000L * 1000L * 60);
	duration -= dur_mins * 1000L * 1000L * 60;
	dur_secs = duration / (1000L * 1000L);
	sprintf(buf, "%02d:%02d:%02d/%02d:%02d:%02d",
		pos_hours, pos_mins, pos_secs,
		dur_hours, dur_mins, dur_secs);
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


/**
 * Draws the overlay window.
 */
static int
mbox_overlay_draw(struct avbox_window *window)
{
	cairo_t *context;
	PangoLayout *msg;
	PangoFontDescription *font_desc;
	struct mbox_overlay * const inst =
		avbox_window_getusercontext(window);
	struct avbox_rect rect;
	struct avbox_rect bar_rect;

	ASSERT(inst != NULL);

	if (!inst->dirty) {
		return 0;
	}

	avbox_window_getcanvassize(window, &rect.w, &rect.h);

	rect.x = 0;
	rect.y = 0;
	bar_rect.x = 50;
	bar_rect.y = 40;
	bar_rect.w = rect.w - 60;
	bar_rect.h = 10;

	/* draw the round window */
	avbox_window_setbgcolor(inst->window, AVBOX_COLOR(0xcccccc00));
	avbox_window_clear(window);
	avbox_window_setcolor(window, AVBOX_COLOR(0xffffffff));
	avbox_window_setbgcolor(window, AVBOX_COLOR(0x0000ffbf));
	avbox_window_roundrectangle(window, &rect, 2, 5);

	/* draw the bar */
	avbox_window_roundrectangle(window, &bar_rect, 2, 10);
	if (inst->position > 0 && inst->duration > 0) {
		bar_rect.w = (bar_rect.w * ((inst->position * 100) /
			inst->duration)) / 100;
		avbox_window_setbgcolor(window, AVBOX_COLOR(0xffffffff));
		avbox_window_roundrectangle(window, &bar_rect, 2, 10);
	}

	if ((context = avbox_window_cairo_begin(window)) != NULL) {

		/* white */
		cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);

		/* draw the icon */
		switch (inst->state) {
		case MBOX_OVERLAY_STATE_READY:
		{
			cairo_move_to(context, 10, 10);
			cairo_line_to(context, 10, rect.h - 15);
			cairo_line_to(context, 40, rect.h - 15);
			cairo_line_to(context, 40, 10);
			cairo_line_to(context, 10, 10);
			cairo_fill(context);
			break;
		}
		case MBOX_OVERLAY_STATE_PLAYING:
		{
			cairo_move_to(context, 10, 10);
			cairo_line_to(context, 10, rect.h - 15);
			cairo_line_to(context, 40, (rect.h - 10) / 2);
			cairo_fill(context);
			break;
		}
		case MBOX_OVERLAY_STATE_PAUSED:
		{
			cairo_move_to(context, 10, 10);
			cairo_line_to(context, 10, rect.h - 10);
			cairo_line_to(context, 20, rect.h - 10);
			cairo_line_to(context, 20, 10);
			cairo_line_to(context, 10, 10);
			cairo_fill(context);
			cairo_move_to(context, 30, 10);
			cairo_line_to(context, 30, rect.h - 10);
			cairo_line_to(context, 40, rect.h - 10);
			cairo_line_to(context, 40, 10);
			cairo_line_to(context, 30, 10);
			cairo_fill(context);
			break;
		}
		default:
			abort();
		}

		/* draw the title */
		if ((msg = pango_cairo_create_layout(context)) != NULL) {
			if ((font_desc = pango_font_description_from_string("Sans Bold 24px")) != NULL) {
				pango_layout_set_font_description(msg, font_desc);
				pango_layout_set_width(msg, (rect.w - 60) * PANGO_SCALE);
				pango_layout_set_height(msg, 25 * PANGO_SCALE);
				pango_layout_set_ellipsize(msg, PANGO_ELLIPSIZE_MIDDLE);
				pango_layout_set_alignment(msg, mbv_get_pango_alignment(inst->alignment));
				pango_layout_set_text(msg, inst->title, -1);
				pango_cairo_update_layout(context, msg);
				pango_font_description_free(font_desc);
				cairo_translate(context, 50, 10);
				pango_cairo_show_layout(context, msg);
				g_object_unref(msg);
			}
		}
		if ((msg = pango_cairo_create_layout(context)) != NULL) {
			if ((font_desc = pango_font_description_from_string("Sans Bold 18px")) != NULL) {
				char duration[20];
				avbox_overlay_formatpos(duration, inst->position, inst->duration);
				pango_layout_set_font_description(msg, font_desc);
				pango_layout_set_width(msg, (250) * PANGO_SCALE);
				pango_layout_set_alignment(msg, mbv_get_pango_alignment(inst->alignment));
				pango_layout_set_text(msg, duration, -1);
				pango_cairo_update_layout(context, msg);
				pango_font_description_free(font_desc);
				cairo_translate(context, rect.w - 250, rect.h - 35);
				pango_cairo_show_layout(context, msg);
				g_object_unref(msg);
			}
		}
		avbox_window_cairo_end(window);
	} else {
		DEBUG_PRINT("overlay", "Could not get cairo context");
	}

	inst->dirty = 0;
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
		inst->dirty = 1;
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
	inst->dirty = 1;
	return 0;
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
			struct timespec tv;
			tv.tv_sec = 1;
			tv.tv_nsec = 0;
			if ((inst->duration_timer = avbox_timer_register(&tv,
				AVBOX_TIMER_TYPE_AUTORELOAD | AVBOX_TIMER_MESSAGE,
				avbox_window_object(inst->window), NULL, NULL)) == -1) {
				LOG_VPRINT_ERROR("Could not register overlay timer: %s",
					strerror(errno));
				avbox_window_hide(inst->window);
			}
		}

		switch (data->status) {
		case MB_PLAYER_STATUS_READY:
			if (inst->duration_timer != -1) {
				avbox_timer_cancel(inst->duration_timer);
				inst->duration_timer = -1;
				if (avbox_window_isvisible(inst->window)) {
					avbox_window_hide(inst->window);
				}
			}
			mbox_overlay_setstate(inst, MBOX_OVERLAY_STATE_READY);
			break;
		case MB_PLAYER_STATUS_BUFFERING:
			if (data->last_status != MB_PLAYER_STATUS_BUFFERING) {
				mbox_overlay_setstate(inst, MBOX_OVERLAY_STATE_READY);
				mbox_overlay_show(inst, 15);
			}
			break;
		case MB_PLAYER_STATUS_PLAYING:
		{
			char *title;
			if ((title = avbox_player_gettitle(inst->player)) != NULL) {
				if (!strncmp(title, "/media/UPnP", 11)) {
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
							if ((tmp = strdup(title + 11)) != NULL) {
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
			inst->dirty = 1;
		} else {
			DEBUG_VPRINT("overlay", "Message for unknown timer %i received!",
				data->id);
		}
		free(data);
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
		avbox_window_hide(inst->window);
		avbox_timer_cancel(inst->dismiss_timer);
		inst->dismiss_timer = -1;
	}

	/* show the overlay */
	avbox_window_show(inst->window);

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

	/* subscribe to player events */
	if (avbox_player_subscribe(player, avbox_window_object(inst->window)) == -1) {
		LOG_VPRINT_ERROR("Could not subscribe to player events: %s",
			strerror(errno));
		avbox_window_destroy(inst->window);
		free(inst);
		return NULL;
	}

	inst->dirty = 1;
	inst->title = strdup("NONE");
	inst->alignment = MBV_ALIGN_LEFT;
	inst->dismiss_timer = -1;
	inst->duration_timer = -1;
	inst->player = player;
	inst->duration = 0;
	inst->position = 0;
	inst->state = MBOX_OVERLAY_STATE_READY;

	return inst;
}


/**
 * Destroy overlay instance.
 */
void
mbox_overlay_destroy(struct mbox_overlay * const inst)
{
	avbox_window_destroy(inst->window);
}
