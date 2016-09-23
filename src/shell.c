#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#define LOG_MODULE "shell"

#include "video.h"
#include "input.h"
#include "mainmenu.h"
#include "player.h"
#include "su.h"
#include "timers.h"
#include "debug.h"
#include "log.h"
#include "alsa-volume.h"
#include "library.h"
#include "ui-progressbar.h"


#define MEDIA_FILE "/mov.mp4"


static int pw = 0, ph = 0;
static struct mbv_window *root_window = NULL;
static struct mbv_window *progress = NULL;
static struct mb_ui_progressbar *progressbar = NULL;
static struct mbp *player = NULL;
static struct mbv_window *volumebar_window = NULL;
static struct mb_ui_progressbar *volumebar = NULL;
static int volumebar_timer_id = -1;
static int input_fd = -1;
static int clock_timer_id = 0;
static pthread_mutex_t screen_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t volume_lock = PTHREAD_MUTEX_INITIALIZER;


/**
 * mbs_get_active_player() -- Gets the currently active player instance.
 */
struct mbp *
mbs_get_active_player(void)
{
	return player;
}


static enum mbt_result
mbs_welcomescreen(int id, void *data)
{
	int w, h;
	time_t now;
	char time_string[256];
	char date_string[256];
	cairo_t *context;
	PangoLayout *layout_time, *layout_date;
	PangoFontDescription *font_desc;

	static char old_time_string[256] = { 0 };

	(void) id;
	(void) data;

	/* format the time string */
	now = time(NULL);
	strftime(time_string, sizeof(time_string), "%l:%M %p",
		localtime(&now));

	/* if the time has not changed there's no need to repaint */
	if (!strcmp(old_time_string, time_string)) {
		return MB_TIMER_CALLBACK_RESULT_CONTINUE;
	} else {
		/* save the time string */
		strcpy(old_time_string, time_string);
	}

	/* format date string */
	strftime(date_string, sizeof(time_string), "%B %d, %Y",
		localtime(&now));

	pthread_mutex_lock(&screen_lock);

	/* mbv_getscreensize(&w, &h); */
	mbv_window_getcanvassize(root_window, &w, &h);

	/* redraw the whole screen */
	mbv_window_clear(root_window, 0x000000ff);
	mbv_window_setcolor(root_window, 0x8080ffff);
	mbv_window_drawline(root_window, 0, h / 2, w - 1, h / 2);

	if ((context = mbv_window_cairo_begin(root_window)) != NULL) {
		if ((layout_time = pango_cairo_create_layout(context)) != NULL) {
			if ((font_desc = pango_font_description_from_string("Sans Bold 128px")) != NULL) {
				pango_layout_set_font_description(layout_time, font_desc);
				pango_layout_set_width(layout_time, w * PANGO_SCALE);
				pango_layout_set_alignment(layout_time, PANGO_ALIGN_CENTER);
				pango_layout_set_text(layout_time, time_string, -1);
				pango_cairo_update_layout(context, layout_time);
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
		mbv_window_cairo_end(root_window);
	} else {
		DEBUG_PRINT("about", "Could not get cairo context");
	}

        mbv_window_show(root_window);

	pthread_mutex_unlock(&screen_lock);

	return MB_TIMER_CALLBACK_RESULT_CONTINUE;
}


static void
mbs_start_clock(void)
{
	struct timespec tv;
	
	DEBUG_PRINT("shell", "Starting clock");

	mbs_welcomescreen(0, NULL);

	tv.tv_sec = 2;
	tv.tv_nsec = 0;
	clock_timer_id = mbt_register(&tv, MB_TIMER_TYPE_AUTORELOAD | MB_TIMER_MESSAGE,
		input_fd, NULL, NULL);
}


static enum mbt_result
mbs_dismissvolumebar(int id, void *data)
{
	pthread_mutex_lock(&volume_lock);

	if (id == volumebar_timer_id) {
		DEBUG_VPRINT("shell", "Dismissing volume indicator (id=%i)",
			id);

		mbv_window_hide(volumebar_window);
		mb_ui_progressbar_destroy(volumebar);
		mbv_window_destroy(volumebar_window);
		volumebar = NULL;
		volumebar_window = NULL;
		volumebar_timer_id = -1;

	} else {
		DEBUG_VPRINT("shell", "Too late to dismiss volume bar (timer id=%i)",
			id);
	}

	pthread_mutex_unlock(&volume_lock);

	return MB_TIMER_CALLBACK_RESULT_STOP;
}


static void
mbs_volumechanged(int volume)
{
	int x, y, w, h;
	int new_timer_id;
	struct timespec tv;
	const int bar_width = 800;

	pthread_mutex_lock(&volume_lock);

	if (volumebar_timer_id == -1) {

		assert(volumebar == NULL);
		assert(volumebar_window == NULL);

		/* calculate volumebar size and location */
		mbv_window_getcanvassize(root_window, &w, &h);
		x = (w / 2) - (bar_width / 2);
		y = h - 150;

		/* create a new window with a progressbar widget */
		volumebar_window = mbv_window_new(NULL, x, y, bar_width, 60);
		if (volumebar_window == NULL) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "shell",
				"Could not create volume indicator window");
			pthread_mutex_unlock(&volume_lock);
			return;
		}
		volumebar = mb_ui_progressbar_new(volumebar_window, 0, 0, bar_width, 60, 0, 100, volume);
		if (volumebar == NULL) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "shell",
				"Could not create volume indicator");
			mbv_window_destroy(volumebar_window);
			pthread_mutex_unlock(&volume_lock);
			return;
		}

		mbv_window_show(volumebar_window);

	} else {
		mb_ui_progressbar_setvalue(volumebar, volume);
	}

	mb_ui_progressbar_update(volumebar);

	/* Register timer to dismiss volume bar */
	tv.tv_sec = 5;
	tv.tv_nsec = 0;
	new_timer_id = mbt_register(&tv, MB_TIMER_TYPE_ONESHOT | MB_TIMER_MESSAGE,
		input_fd, NULL, NULL);
	if (new_timer_id == -1) {
		pthread_mutex_unlock(&volume_lock);
		LOG_PRINT(MB_LOGLEVEL_ERROR, "shell", "Could not register volume bar timer");

		/* Hide the window only if there's not a timer already
		 * running. Otherwise it'll get hidden when the timer
		 * expires */
		if (volumebar_timer_id == -1) {
			mbv_window_hide(volumebar_window);
			mb_ui_progressbar_destroy(volumebar);
			mbv_window_destroy(volumebar_window);
			volumebar = NULL;
			volumebar_window = NULL;
			return;
		}
	}

	DEBUG_VPRINT("shell", "Registered volumebar timer (id=%i)", new_timer_id);

	/* if there is a timer running already cancel it */
	if (volumebar_timer_id != -1) {
		mbt_cancel(volumebar_timer_id);
	}

	volumebar_timer_id = new_timer_id;

	pthread_mutex_unlock(&volume_lock);
}


/**
 * mbs_playerstatuschanged() -- Handle player state change events
 */
static void
mbs_playerstatuschanged(struct mbp *inst,
	enum mb_player_status status, enum mb_player_status last_status)
{
	if (inst == player) {
		if (last_status == MB_PLAYER_STATUS_BUFFERING && status != MB_PLAYER_STATUS_BUFFERING) {

			DEBUG_PRINT("shell", "Destroying progress bar");

			if (progress != NULL) {

				assert(progressbar != NULL);

				/* hide and destroy progress bar */
				mbv_window_hide(progress);
				mb_ui_progressbar_destroy(progressbar);
				mbv_window_destroy(progress);
				progressbar = NULL;
				progress = NULL;
			}

		} else if (last_status == MB_PLAYER_STATUS_PAUSED && status != MB_PLAYER_STATUS_PAUSED) {
			mb_player_showoverlaytext(player, "", 1,
				MBV_ALIGN_LEFT);
		}

		/* if we're out of the READY state then cancel the clock
		 * timer */
		if (clock_timer_id != 0 && status != MB_PLAYER_STATUS_READY) {
			DEBUG_PRINT("shell", "Stoping clock timer");
			if (mbt_cancel(clock_timer_id) == 0) {
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
				mbs_start_clock();
			}
			break;

		case MB_PLAYER_STATUS_BUFFERING:
			if (last_status != MB_PLAYER_STATUS_BUFFERING) {
				DEBUG_PRINT("shell", "Player state changed to BUFFERING");
			}

			/* We null test progress here instead of (last_status != MB_PLAYER_STATUS_BUFFERING)
			 * because if for any reason this block bails out early we'll want it to run again
			 * next time */
			if (progress == NULL) {
				int sw, sh, px, py;

				DEBUG_PRINT("shell", "Initializing progress bar");

				assert(progressbar == NULL);

				mbv_window_clear(root_window, 0x000000ff);
				mbv_window_update(root_window);

				mbv_window_getsize(root_window, &sw, &sh);

				pw = (sw * 70) / 100;
				ph = 30;
				px = (sw / 2) - (pw / 2);
				py = (sh / 2) - (ph / 2);

				/* create a window for the progress bar */
				if ((progress = mbv_window_new(NULL, px, py, pw, ph)) == NULL) {
					LOG_PRINT_ERROR("Could not create progressbar window");
					break;
				}

				/* create the progressbar widget */
				if ((progressbar = mb_ui_progressbar_new(progress, 0, 0, pw, ph, 0, 100, 0)) == NULL) {
					LOG_PRINT_ERROR("Could not create progressbar widget");
					mbv_window_destroy(progress);
					break;
				}

				/* show the progress bar */
				mb_ui_progressbar_update(progressbar);
				mbv_window_show(progress);

			} else {
				assert(progress != NULL);
				assert(progressbar != NULL);

				/* update the progress bar */
				mb_ui_progressbar_setvalue(progressbar, mb_player_bufferstate(inst));
				mb_ui_progressbar_update(progressbar);
				mbv_window_update(progress);
			}
			break;

		case MB_PLAYER_STATUS_PLAYING:
			assert(progress == NULL);
			DEBUG_PRINT("shell", "Player state changed to PLAYING");
			break;

		case MB_PLAYER_STATUS_PAUSED:
			assert(progress == NULL);
			DEBUG_PRINT("shell", "Player state changed to PAUSED");

			mb_player_showoverlaytext(player, "  PAUSED", 1000,
				MBV_ALIGN_LEFT);
			mb_player_update(inst);
			break;
		}
	}
}


/**
 * mbs_init() -- Initialize the MediaBox shell
 */
int
mbs_init(void)
{
	/* create the root window */
        root_window = mbv_getrootwindow();
	if (root_window == NULL) {
		fprintf(stderr, "Could not create root window\n");
		return -1;
	}

	/* initialize the volume control */
	if (mb_alsa_volume_init(mbs_volumechanged) != 0) {
		fprintf(stderr, "shell: Could not initialize volume control");
		return -1;
	}

	/* initialize main media player */
	player = mb_player_new(NULL);
	if (player == NULL) {
		fprintf(stderr, "Could not initialize main media player\n");
		return -1;
	}

	/* register for status updates */
	mb_player_add_status_callback(player, mbs_playerstatuschanged);
	mbv_window_show(root_window);

	return 0;
}


int
mbs_show_dialog(void)
{
	int quit = 0;
	struct mb_message *message;

	/* grab the input device */
	if ((input_fd = mbi_grab_input()) == -1) {
		fprintf(stderr, "mbs_show() -- mbi_grab_input failed\n");
		return -1;
	}

	mbs_start_clock();

	/* run the message loop */
	while (!quit && (message = mbi_getmessage(input_fd)) != NULL) {
		switch (message->msg) {
		case MBI_EVENT_KBD_Q:
		case MBI_EVENT_QUIT:
		{
#ifndef NDEBUG
			close(input_fd);
			quit = 1;
#else
			abort();
#endif
			break;
		}
		case MBI_EVENT_MENU:
		{
			if (mb_mainmenu_init() == -1) {
				fprintf(stderr, "Could not initialize main menu\n");
				break;
			}
			if (mb_mainmenu_showdialog() == -1) {
				fprintf(stderr, "mbs: Main Menu dismissed\n");
			}
			mb_mainmenu_destroy();
			break;
		}
		case MBI_EVENT_KBD_P:
		case MBI_EVENT_PLAY:
		{
			switch (mb_player_getstatus(player)) {
			case MB_PLAYER_STATUS_READY:
			{
				const char *media_file = mb_player_getmediafile(player);
				if (media_file == NULL) {
					media_file = MEDIA_FILE;
				} else {
					DEBUG_VPRINT("shell", "Playing '%s' from memory",
						media_file);
				}
				mb_player_play(player, media_file);
				break;
			}
			case MB_PLAYER_STATUS_BUFFERING:
			{
				/* this should never happen since this state is
				 * a temporary state in mb_player_player(). */
				abort();
				break;
			}
			case MB_PLAYER_STATUS_PLAYING:
			{
				mb_player_pause(player);
				break;
			}
			case MB_PLAYER_STATUS_PAUSED:
			{
				mb_player_play(player, NULL);
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
			if (mb_player_getstatus(player) != MB_PLAYER_STATUS_READY) {
				(void) mb_player_stop(player);
			}
			break;
		}
		case MBI_EVENT_PREV:
		{
			enum mb_player_status status;
			status = mb_player_getstatus(player);
			if (status == MB_PLAYER_STATUS_PLAYING || status == MB_PLAYER_STATUS_PAUSED) {
				mb_player_seek_chapter(player, -1);
			}
			break;
		}
		case MBI_EVENT_NEXT:
		{
			enum mb_player_status status;
			status = mb_player_getstatus(player);
			if (status == MB_PLAYER_STATUS_PLAYING || status == MB_PLAYER_STATUS_PAUSED) {
				mb_player_seek_chapter(player, 1);
			}
			break;
		}
		case MBI_EVENT_KBD_I:
		case MBI_EVENT_INFO:
		{
			char *title = mb_player_gettitle(player);
			if (title != NULL) {
				mb_player_showoverlaytext(player, title, 15,
					MBV_ALIGN_CENTER);
				free(title);
			}
			break;
		}
		case MBI_EVENT_VOLUME_UP:
		{
			int volume;
			volume = mb_alsa_volume_get();
			volume += 10;
			if (volume > 100) {
				volume = 100;
			}
			mb_alsa_volume_set(volume);
			break;
		}
		case MBI_EVENT_VOLUME_DOWN:
		{
			int volume;
			volume = mb_alsa_volume_get();
			volume -= 10;
			if (volume < 0) {
				volume = 0;
			}
			mb_alsa_volume_set(volume);
			break;
		}
		case MBI_EVENT_TIMER:
		{
			struct mbt_timer_data *timer_data;
			timer_data = (struct mbt_timer_data*) message->payload;

			/* DEBUG_VPRINT("shell", "Received timer message id=%i",
				timer_data->id); */

			if (timer_data->id == clock_timer_id) {
				mbs_welcomescreen(timer_data->id, timer_data->data);
			} else if (timer_data->id == volumebar_timer_id) {
				mbs_dismissvolumebar(timer_data->id, timer_data->data);
			}
			break;
		}
		default:
			DEBUG_VPRINT("shell", "Received event %i", (int) message->msg);
			break;
		}
		mb_player_update(player);
		free(message);
	}

	fprintf(stderr, "mbs: Exiting\n");
	return 0;
}


void
mbs_reboot(void)
{
	if (mb_su_gainroot() == 0) {
		close(input_fd);
		if (system("systemctl stop avmount") != 0) {
			fprintf(stderr, "shell: systemctl stop avmount failed\n");
			return;
		}
		if (system("systemctl reboot") != 0) {
			fprintf(stderr, "shell: systemctl reboot failed\n");
		}
	}
}


void
mbs_destroy(void)
{
	mb_player_destroy(player);
}

