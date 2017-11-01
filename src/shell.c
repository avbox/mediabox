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
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define LOG_MODULE "shell"

#include "lib/ui/video.h"
#include "lib/ui/progressview.h"
#include "lib/ui/player.h"
#include "lib/ui/input.h"
#include "lib/su.h"
#include "lib/timers.h"
#include "lib/debug.h"
#include "lib/log.h"
#include "lib/volume.h"
#include "library.h"
#include "lib/dispatch.h"
#include "lib/application.h"
#include "lib/settings.h"
#include "mainmenu.h"
#include "discovery.h"
#include "downloads-backend.h"
#include "library-backend.h"
#include "overlay.h"


#define MEDIA_FILE "/mov.mp4"


static int pw = 0, ph = 0;
static struct avbox_object *dispatch_object = NULL;
static struct avbox_window *main_window = NULL;
static struct avbox_window *progress = NULL;
static struct avbox_progressview *progressbar = NULL;
static struct avbox_player *player = NULL;
static struct avbox_window *volumebar_window = NULL;
static struct avbox_progressview *volumebar = NULL;
static struct mbox_mainmenu *mainmenu = NULL;
static struct mbox_overlay *overlay = NULL;
static int volumebar_timer_id = -1;
static int clock_timer_id = -1;
static char time_string[256] = "";
static char date_string[256] = "";


/**
 * Gets the currently active player instance.
 */
struct avbox_player *
mbox_shell_getactiveplayer(void)
{
	return player;
}


/**
 * Gets the shell's message queue fd.
 */
struct avbox_object *
mbox_shell_getqueue(void)
{
	return dispatch_object;
}


/**
 * Draws the welcome screen.
 */
static int
mbox_shell_draw(struct avbox_window *window)
{
	int w, h;
	cairo_t *context;
	PangoLayout *layout_time, *layout_date;
	PangoFontDescription *font_desc;

	/* DEBUG_VPRINT("shell", "mbox_shell_draw(0x%p)",
		window); */

	/* assert(avbox_window_isvisible(window)); */

	avbox_window_getcanvassize(window, &w, &h);
	avbox_window_clear(window);
	avbox_window_drawline(window, 0, h / 2, w - 1, h / 2);

	if ((context = avbox_window_cairo_begin(window)) != NULL) {
		if ((layout_time = pango_cairo_create_layout(context)) != NULL) {
			if ((font_desc = pango_font_description_from_string("Sans Bold 128px")) != NULL) {
				pango_layout_set_font_description(layout_time, font_desc);
				pango_layout_set_width(layout_time, w * PANGO_SCALE);
				pango_layout_set_alignment(layout_time, PANGO_ALIGN_CENTER);
				pango_layout_set_text(layout_time, time_string, -1);
				pango_cairo_update_layout(context, layout_time);
				pango_font_description_free(font_desc);
			}
		}
		if ((layout_date = pango_cairo_create_layout(context)) != NULL) {
			pango_layout_set_font_description(layout_date, mbv_getdefaultfont());
			pango_layout_set_width(layout_date, w * PANGO_SCALE);
			pango_layout_set_alignment(layout_date, PANGO_ALIGN_CENTER);
			pango_layout_set_text(layout_date, date_string, -1);
			pango_cairo_update_layout(context, layout_date);
		}

		cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);

		if (layout_time != NULL && layout_date != NULL) {
			cairo_translate(context, 0, (h / 2) - (10 + 128 + 48));
			pango_cairo_show_layout(context, layout_time);
			cairo_translate(context, 0, 128 + 10);
			pango_cairo_show_layout(context, layout_date);
		}

		if (layout_time != NULL) {
			g_object_unref(layout_time);
		}
		if (layout_date != NULL) {
			g_object_unref(layout_date);
		}
		avbox_window_cairo_end(window);
	} else {
		DEBUG_PRINT("about", "Could not get cairo context");
	}

	return 0;
}


/**
 * Draws the shutdown dialog */
static int
mbox_shell_shutdowndraw(struct avbox_window *window)
{
	int w, h;
	cairo_t *context;
	PangoLayout *msg;
	PangoFontDescription *font_desc;

	DEBUG_PRINT("shell", "Drawing shutdown dialog");

	avbox_window_getcanvassize(window, &w, &h);
	avbox_window_clear(window);

	if ((context = avbox_window_cairo_begin(window)) != NULL) {
		if ((msg = pango_cairo_create_layout(context)) != NULL) {
			if ((font_desc = pango_font_description_from_string("Sans Bold 48px")) != NULL) {
				pango_layout_set_font_description(msg, font_desc);
				pango_layout_set_width(msg, w * PANGO_SCALE);
				pango_layout_set_alignment(msg, PANGO_ALIGN_CENTER);
				pango_layout_set_text(msg, "Shutting Down", -1);
				pango_cairo_update_layout(context, msg);
				pango_font_description_free(font_desc);
				cairo_translate(context, 0, 0);
				cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
				pango_cairo_show_layout(context, msg);
				g_object_unref(msg);
			}
		}
		avbox_window_cairo_end(window);
	} else {
		DEBUG_PRINT("about", "Could not get cairo context");
	}
	return 0;
}


/**
 * This is the callback function for the timer
 * that updates the clock on the welcome screen.
 */
static enum avbox_timer_result
mbox_shell_welcomescreen(int id, void *data)
{
	time_t now;
	static char old_time_string[256] = { 0 };

	(void) id;
	(void) data;

	/* format the time string */
	now = time(NULL);
	strftime(time_string, sizeof(time_string), "%I:%M %p",
		localtime(&now));

	/* if the time has not changed there's no need to redraw */
	if (!strcmp(old_time_string, time_string)) {
		return AVBOX_TIMER_CALLBACK_RESULT_CONTINUE;
	} else {
		/* save the time string */
		strcpy(old_time_string, time_string);
	}

	/* format date string */
	strftime(date_string, sizeof(time_string), "%B %d, %Y",
		localtime(&now));

	/* redraw the window */
        avbox_window_update(main_window);

	return AVBOX_TIMER_CALLBACK_RESULT_CONTINUE;
}


static void
mbox_shell_startclock(void)
{
	struct timespec tv;
	
	DEBUG_PRINT("shell", "Starting clock");

	mbox_shell_welcomescreen(0, NULL);

	tv.tv_sec = 2;
	tv.tv_nsec = 0;
	clock_timer_id = avbox_timer_register(&tv,
		AVBOX_TIMER_TYPE_AUTORELOAD | AVBOX_TIMER_MESSAGE,
		dispatch_object, NULL, NULL);
}


static enum avbox_timer_result
mbox_shell_dismissvolumebar(int id, void *data)
{
	if (id == volumebar_timer_id) {
		DEBUG_VPRINT("shell", "Dismissing volume indicator (id=%i)",
			id);

		avbox_window_hide(volumebar_window);
		avbox_progressview_destroy(volumebar);
		avbox_window_destroy(volumebar_window);
		volumebar = NULL;
		volumebar_window = NULL;
		volumebar_timer_id = -1;

	} else {
		DEBUG_VPRINT("shell", "Too late to dismiss volume bar (timer id=%i)",
			id);
	}

	return AVBOX_TIMER_CALLBACK_RESULT_STOP;
}


/**
 * Handles volume changes.
 */
static void
mbox_shell_volumechanged(int volume)
{
	int x, y, w, h;
	int new_timer_id;
	struct timespec tv;

	if (volumebar_timer_id == -1) {
		int bar_width;
		const int bar_height = 40;
		struct avbox_window *root_window;

		assert(volumebar == NULL);
		assert(volumebar_window == NULL);

		/* calculate volumebar size and location */
		root_window = avbox_video_getrootwindow(0);
		avbox_window_getcanvassize(root_window, &w, &h);
		bar_width = (w * 70) / 100;
		x = (w / 2) - (bar_width / 2);
		y = h - 150;

		/* create a new window with a progressbar widget */
		volumebar_window = avbox_window_new(NULL, "volumebar",
			AVBOX_WNDFLAGS_NONE, x, y, bar_width, bar_height, NULL, NULL, NULL);
		if (volumebar_window == NULL) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "shell",
				"Could not create volume indicator window");
			return;
		}
		volumebar = avbox_progressview_new(volumebar_window, 0, 0,
			bar_width, bar_height, 0, 100, volume);
		if (volumebar == NULL) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "shell",
				"Could not create volume indicator");
			avbox_window_destroy(volumebar_window);
			return;
		}

		avbox_window_show(volumebar_window);

	} else {
		avbox_progressview_setvalue(volumebar, volume);
	}

	/* draw the progress bar */
	avbox_window_update(volumebar_window);

	/* Register timer to dismiss volume bar */
	tv.tv_sec = 5;
	tv.tv_nsec = 0;
	new_timer_id = avbox_timer_register(&tv, AVBOX_TIMER_TYPE_ONESHOT | AVBOX_TIMER_MESSAGE,
		dispatch_object, NULL, NULL);
	if (new_timer_id == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "shell", "Could not register volume bar timer");

		/* Hide the window only if there's not a timer already
		 * running. Otherwise it'll get hidden when the timer
		 * expires */
		if (volumebar_timer_id == -1) {
			avbox_window_hide(volumebar_window);
			avbox_progressview_destroy(volumebar);
			avbox_window_destroy(volumebar_window);
			volumebar = NULL;
			volumebar_window = NULL;
			return;
		}
	}

	DEBUG_VPRINT("shell", "Registered volumebar timer (id=%i)", new_timer_id);

	/* if there is a timer running already cancel it */
	if (volumebar_timer_id != -1) {
		avbox_timer_cancel(volumebar_timer_id);
	}

	volumebar_timer_id = new_timer_id;
}


/**
 * Handle player state change events
 */
static void
mbox_shell_playerstatuschanged(struct avbox_player *inst,
	enum avbox_player_status status, enum avbox_player_status last_status)
{
	if (inst == player) {
		if (last_status == MB_PLAYER_STATUS_READY && status != MB_PLAYER_STATUS_READY) {
			if (mainmenu != NULL) {
				char * last_file = avbox_player_getmediafile(inst);
				if (last_file != NULL) {
					avbox_settings_setstring("last_file", last_file);
					free(last_file);
				}
				mbox_mainmenu_destroy(mainmenu);
				mainmenu = NULL;
			}
		}
		if (last_status == MB_PLAYER_STATUS_BUFFERING &&
			status != MB_PLAYER_STATUS_BUFFERING) {

			DEBUG_PRINT("shell", "Destroying progress bar");

			if (progress != NULL) {

				assert(progressbar != NULL);

				/* hide and destroy progress bar */
				avbox_window_hide(progress);
				avbox_progressview_destroy(progressbar);
				avbox_window_destroy(progress);
				progressbar = NULL;
				progress = NULL;
			}

		}

		/* if we're out of the READY state then cancel the clock
		 * timer */
		if (clock_timer_id != 0 && status != MB_PLAYER_STATUS_READY) {
			DEBUG_PRINT("shell", "Hiding main window");

			/* hide the welcome screen window */
			avbox_window_hide(main_window);

			DEBUG_PRINT("shell", "Stoping clock timer");
			if (avbox_timer_cancel(clock_timer_id) == 0) {
				DEBUG_PRINT("shell", "Cancelled clock timer");
				clock_timer_id = 0;
			} else {
				DEBUG_VPRINT("shell", "Could not cancel clock timer (id=%i)",
					clock_timer_id);
			}
		}

		switch (status) {
		case MB_PLAYER_STATUS_READY:
			DEBUG_PRINT("shell", "Player state changed to READY");
			if (clock_timer_id == 0) {
				DEBUG_PRINT("shell", "Showing main window");
				assert(!avbox_window_isvisible(main_window));
				avbox_window_show(main_window);
				mbox_shell_startclock();
			}
			break;

		case MB_PLAYER_STATUS_BUFFERING:
			/* We null test progress here instead of (last_status != MB_PLAYER_STATUS_BUFFERING)
			 * because if for any reason this block bails out early we'll want it to run again
			 * next time */
			if (progress == NULL) {
				int sw, sh, px, py;
				struct avbox_window *root_window;

				DEBUG_PRINT("shell", "Initializing progress bar");

				assert(progressbar == NULL);

				/* clear the root window */
				/* TODO: This is not needed now */
				root_window = avbox_video_getrootwindow(0);
				assert(root_window != NULL);
				avbox_window_clear(root_window);
				avbox_window_update(root_window);

				avbox_window_getsize(root_window, &sw, &sh);

				pw = (sw * 70) / 100;
				ph = 30;
				px = (sw / 2) - (pw / 2);
				py = (sh / 2) - (ph / 2);

				/* create a window for the progress bar */
				if ((progress = avbox_window_new(NULL, "progressbar",
					AVBOX_WNDFLAGS_NONE, px, py, pw, ph, NULL, NULL, NULL)) == NULL) {
					LOG_PRINT_ERROR("Could not create progressbar window");
					break;
				}

				/* create the progressbar widget */
				if ((progressbar = avbox_progressview_new(progress, 0, 0, pw, ph, 0, 100, 0)) == NULL) {
					LOG_PRINT_ERROR("Could not create progressbar widget");
					avbox_window_destroy(progress);
					break;
				}

				/* show the progress bar */
				avbox_progressview_update(progressbar);
				avbox_window_show(progress);

			} else {
				assert(progress != NULL);
				assert(progressbar != NULL);

				/* update the progress bar */
				avbox_progressview_setvalue(progressbar, avbox_player_bufferstate(inst));
				avbox_progressview_update(progressbar);
				avbox_window_update(progress);
			}
			break;

		case MB_PLAYER_STATUS_PLAYING:
		{
			assert(progress == NULL);
			DEBUG_PRINT("shell", "Player state changed to PLAYING");
			break;
		}
		case MB_PLAYER_STATUS_PAUSED:
			assert(progress == NULL);
			DEBUG_PRINT("shell", "Player state changed to PAUSED");
			avbox_player_update(inst);
			break;
		}
	}
}


static void
mbox_shell_shutdown(void);


/**
 * Handle application events.
 */
static int
mbox_shell_appevent(void *context, int event)
{
	switch (event) {
	case AVBOX_APPEVENT_QUIT:
		mbox_shell_shutdown();
		break;
	}
	return 0;
}


/**
 * Destroy the shell.
 */
static void
mbox_shell_shutdown(void)
{
	int w, h;
	struct avbox_window *msgwin, *root_window;

	DEBUG_PRINT("shell", "Shutting down");

	/* release input */
	avbox_input_release(dispatch_object);

	/* cancel all timers */
	if (volumebar_timer_id != -1) {
		avbox_timer_cancel(volumebar_timer_id);
	}
	if (clock_timer_id != -1) {
		avbox_timer_cancel(clock_timer_id);
	}

	/* dismiss the overlay */
	mbox_overlay_destroy(overlay);

	/* destroy player */
	if (player != NULL) {
		if (avbox_player_unsubscribe(player, dispatch_object) == -1) {
			LOG_VPRINT_ERROR("Could not unsubscribe from player events: %s",
				strerror(errno));
		}
		avbox_object_destroy(avbox_player_object(player));
	}

	/* unsubscribe from app events */
	if (avbox_application_unsubscribe(mbox_shell_appevent, NULL) == -1) {
		LOG_VPRINT_ERROR("Could not unsubscribe from app events: %s",
			strerror(errno));
	}

	avbox_volume_shutdown();
	avbox_object_destroy(dispatch_object);

	/* destroy the main window */
	avbox_window_destroy(main_window);

	/* clear the root window the root window */
	root_window = avbox_video_getrootwindow(0);
	assert(root_window != NULL);
	avbox_window_getcanvassize(root_window, &w, &h);
	avbox_window_clear(root_window);
	avbox_window_update(root_window);

	/* draw a shutdown dialog, since there's no main
	 * window this will stay on the display after
	 * it is destroyed */
        msgwin = avbox_window_new(NULL, "shutdown",
		AVBOX_WNDFLAGS_NONE, (w/2) - 240, (h/2) - 30, 480, 60,
		NULL, &mbox_shell_shutdowndraw, NULL);
	if (msgwin != NULL) {
		DEBUG_PRINT("shell", "Showing shutdown message");
		avbox_window_setbgcolor(msgwin, AVBOX_COLOR(0x000000ff));
		avbox_window_setcolor(msgwin, AVBOX_COLOR(0x8080ffff));
		avbox_window_show(msgwin);
		avbox_window_destroy(msgwin);
	} else {
		LOG_PRINT_ERROR("Could not show shutdown dialog");
	}

	/* shutdown services */
	avbox_discovery_shutdown();
	mb_downloadmanager_destroy();

	DEBUG_PRINT("shell", "Shell shutdown complete");
}


/**
 * Handle incomming messages.
 */
static int
mbox_shell_handler(void *context, struct avbox_message *msg)
{
	int ret = AVBOX_DISPATCH_OK;

	(void) context;

	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_INPUT:
	{ 
		struct avbox_input_message *event =
			avbox_message_payload(msg);

		DEBUG_PRINT("shell", "Input event received");

		switch (event->msg) {
		case MBI_EVENT_KBD_Q:
			avbox_application_quit(0);
			break;
		case MBI_EVENT_KBD_SPACE:
		case MBI_EVENT_MENU:
		{

			DEBUG_PRINT("shell", "MENU key pressed");

			/* the main menu is already visible */
			if (mainmenu != NULL) {
				break;
			}

			/* initialize meain menu */
			if ((mainmenu = mbox_mainmenu_new(dispatch_object)) == NULL) {
				LOG_PRINT_ERROR("Could not initialize main menu!");
				break;
			}

			/* attempt to show the main menu */
			if (mbox_mainmenu_show(mainmenu) == -1) {
				LOG_PRINT_ERROR("Could not show main menu!");
				mbox_mainmenu_destroy(mainmenu);
			}
			break;
		}
		case MBI_EVENT_KBD_P:
		case MBI_EVENT_PLAY:
		{
			switch (avbox_player_getstatus(player)) {
			case MB_PLAYER_STATUS_READY:
			{
				char *media_file = avbox_player_getmediafile(player);
				if (media_file == NULL) {
					media_file = avbox_settings_getstring("last_file");
					if (media_file == NULL) {
						media_file = strdup(MEDIA_FILE);
					}
				} else {
					DEBUG_VPRINT("shell", "Playing '%s' from memory",
						media_file);
				}
				if (media_file != NULL) {
					avbox_player_play(player, media_file);
					free(media_file);
				} else {
					LOG_PRINT_ERROR("No file to play");
				}
				break;
			}
			case MB_PLAYER_STATUS_BUFFERING:
			{
				DEBUG_PRINT(LOG_MODULE, "Received play while buffering");
				break;
			}
			case MB_PLAYER_STATUS_PLAYING:
			{
				avbox_player_pause(player);
				break;
			}
			case MB_PLAYER_STATUS_PAUSED:
			{
				avbox_player_play(player, NULL);
				break;
			}
			default:
				break;
			}
			break;
		}
		case MBI_EVENT_KBD_S:
		case MBI_EVENT_STOP:
		{
			if (avbox_player_getstatus(player) != MB_PLAYER_STATUS_READY) {
				(void) avbox_player_stop(player);
			}
			break;
		}
		case MBI_EVENT_PREV:
		{
			enum avbox_player_status status;
			status = avbox_player_getstatus(player);
			if (status == MB_PLAYER_STATUS_PLAYING || status == MB_PLAYER_STATUS_PAUSED) {
				avbox_player_seek_chapter(player, -1);
			}
			break;
		}
		case MBI_EVENT_NEXT:
		{
			enum avbox_player_status status;
			status = avbox_player_getstatus(player);
			if (status == MB_PLAYER_STATUS_PLAYING || status == MB_PLAYER_STATUS_PAUSED) {
				avbox_player_seek_chapter(player, 1);
			}
			break;
		}
		case MBI_EVENT_KBD_I:
		case MBI_EVENT_INFO:
		{
			mbox_overlay_show(overlay, 15);
			break;
		}
		case MBI_EVENT_VOLUME_UP:
		{
			int volume;
			volume = avbox_volume_get();
			volume += 5;
			if (volume > 100) {
				volume = 100;
			}
			avbox_volume_set(volume);
			break;
		}
		case MBI_EVENT_VOLUME_DOWN:
		{
			int volume;
			volume = avbox_volume_get();
			volume -= 5;
			if (volume < 0) {
				volume = 0;
			}
			avbox_volume_set(volume);
			break;
		}
		default:
			DEBUG_VPRINT("shell", "Received event %i", (int) event->msg);
			/* since we're the root window we need to
			 * free the input event even if we don't process it
			 * so we always return AVBOX_DISPATCH_OK */
			break;
		}

		avbox_input_eventfree(event);
		break;
	}
	case AVBOX_MESSAGETYPE_TIMER:
	{
		struct avbox_timer_data * const timer_data =
			avbox_message_payload(msg);

		/* DEBUG_VPRINT("shell", "Received timer message id=%i",
			timer_data->id); */

		if (timer_data->id == clock_timer_id) {
			mbox_shell_welcomescreen(timer_data->id, timer_data->data);
		} else if (timer_data->id == volumebar_timer_id) {
			mbox_shell_dismissvolumebar(timer_data->id, timer_data->data);
		}

		/* free payload */
		free(timer_data);

		break;
	} 
	case AVBOX_MESSAGETYPE_VOLUME:
	{
		int *vol;
		vol = avbox_message_payload(msg);
		mbox_shell_volumechanged(*vol);
		break;
	}
	case AVBOX_MESSAGETYPE_PLAYER:
	{
		struct avbox_player_status_data * const status_data =
			avbox_message_payload(msg);
		mbox_shell_playerstatuschanged(status_data->sender,
			status_data->status, status_data->last_status);
		free(status_data);
		break;
	}
	case AVBOX_MESSAGETYPE_DISMISSED:
	{
		DEBUG_PRINT("shell", "Received DISMISSED message");

		if (avbox_message_payload(msg) == mainmenu) {
			mbox_mainmenu_destroy(mainmenu);
			mainmenu = NULL;
		}
		break;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		break;
	}
	default:
		DEBUG_VPRINT("shell", "Invalid message type: %i",
			avbox_message_id(msg));
		break;
	}
	return ret;
}


/**
 * Initialize the MediaBox shell
 */
int
mbox_shell_init(int launch_avmount, int launch_mediatomb)
{
	int w, h;
	struct avbox_window *root_window;

	/* initialize the library backend */
	if (mb_library_backend_init(launch_avmount, launch_mediatomb) == -1) {
		fprintf(stderr, "Could not initialize library backend\n");
		return -1;
	}

	/* initialize download manager */
	if (mb_downloadmanager_init() == -1) {
		LOG_PRINT_ERROR("Could not initialize download manager!");
		return -1;
	}

	/* initialize the discovery service */
	if (avbox_discovery_init() == -1) {
		LOG_PRINT_ERROR("Could not start discovery service");
		mb_downloadmanager_destroy();
		return -1;
	}

	/* get the screen size in pixels (that's the
	 * size of the root window */
	root_window = avbox_video_getrootwindow(0);
	assert(root_window != NULL);
	avbox_window_getcanvassize(root_window, &w, &h);

	/* create the welcome screen window */
        main_window = avbox_window_new(NULL, "welcome",
		AVBOX_WNDFLAGS_NONE, 0, 0, w, h,
		NULL, &mbox_shell_draw, NULL);
	if (main_window == NULL) {
		LOG_PRINT_ERROR("Could not create root window!");
		return -1;
	}
	avbox_window_setbgcolor(main_window, AVBOX_COLOR(0x000000ff));
	avbox_window_setcolor(main_window, AVBOX_COLOR(0x8080ffff));

	/* initialize main media player */
	player = avbox_player_new(NULL);
	if (player == NULL) {
		LOG_PRINT_ERROR("Could not initialize main player!");
		avbox_window_destroy(main_window);
		return -1;
	}

	/* create the overlay */
	if ((overlay = mbox_overlay_new(player)) == NULL) {
		LOG_VPRINT_ERROR("Could not create overlay: %s",
			strerror(errno));
		avbox_window_destroy(main_window);
		return -1;
	}

	/* create dispatch object */
	if ((dispatch_object = avbox_object_new(mbox_shell_handler, NULL)) == NULL) {
		LOG_VPRINT_ERROR("Could not create dispatch object: %s",
			strerror(errno));
		avbox_object_destroy(avbox_player_object(player));
		avbox_window_destroy(main_window);
		return -1;
	}

	/* initialize the volume control */
	if (avbox_volume_init(dispatch_object) != 0) {
		LOG_PRINT_ERROR("Could not initialize volume control!");
		avbox_object_destroy(dispatch_object);
		avbox_object_destroy(avbox_player_object(player));
		avbox_window_destroy(main_window);
		return -1;
	}

	/* subscribe to player notifications */
	if (avbox_player_subscribe(player, dispatch_object) == -1) {
		LOG_PRINT_ERROR("Could not reqister notification object");
		avbox_object_destroy(dispatch_object);
		avbox_object_destroy(avbox_player_object(player));
		avbox_window_destroy(main_window);
		return -1;
	}

	/* subscribe to application notifications */
	if (avbox_application_subscribe(mbox_shell_appevent, NULL) == -1) {
		LOG_VPRINT_ERROR("Could not subscribe to app events: %s",
			strerror(errno));
		avbox_object_destroy(dispatch_object);
		avbox_object_destroy(avbox_player_object(player));
		avbox_window_destroy(main_window);
		return -1;
	}

	return 0;
}


/**
 * Start the shell.
 */
int
mbox_shell_show(void)
{
	/* start the clock timer */
	avbox_window_show(main_window);
	mbox_shell_startclock();

	/* grab the input device */
	if (avbox_input_grab(dispatch_object) == -1) {
		LOG_VPRINT_ERROR("Could not grab input: %s",
			strerror(errno));
		return -1;
	}

	return 0;
}


void
mbox_shell_reboot(void)
{
	if (avbox_gainroot() == 0) {
		avbox_input_release(dispatch_object);
		avbox_object_destroy(dispatch_object);
		if (system("systemctl stop avmount") != 0) {
			fprintf(stderr, "shell: systemctl stop avmount failed\n");
			return;
		}
		if (system("systemctl reboot") != 0) {
			fprintf(stderr, "shell: systemctl reboot failed\n");
		}
		avbox_droproot();
	}
}
