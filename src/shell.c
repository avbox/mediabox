#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#include "video.h"
#include "input.h"
#include "mainmenu.h"
#include "player.h"
#include "su.h"
#include "timers.h"
#include "debug.h"


#define MEDIA_FILE "/mov.mp4"


static int pw = 0, ph = 0;
static struct mbv_window *root_window = NULL;
static struct mbv_window *progress = NULL;
static struct mbp *player = NULL;
static int input_fd = -1;
static int clock_timer_id = 0;


/**
 * mbs_get_active_player() -- Gets the currently active player instance.
 */
struct mbp *
mbs_get_active_player(void)
{
	return player;
}


/**
 * mbs_clearscreen() -- Clears the screen and displas a blue line
 * accross it. In the future we'll do something nicer.
 */
static void
mbs_clearscreen(void)
{
	DEBUG_PRINT("shell", "Clear screen");

	/* show the root window */
	mbv_window_clear(root_window, 0x000000FF);
        mbv_window_update(root_window);
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

	(void) id;
	(void) data;

	mbv_getscreensize(&w, &h);

	/* show the root window */
	mbv_window_clear(root_window, 0x000000ff);
	mbv_window_setcolor(root_window, 0x8080ffff);
	mbv_window_drawline(root_window, 0, h / 2, w - 1, h / 2);

	now = time(NULL);
	strftime(time_string, sizeof(time_string), "%l:%M %p",
		localtime(&now));
	strftime(date_string, sizeof(time_string), "%B %d, %Y",
		localtime(&now));

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

	return MB_TIMER_CALLBACK_RESULT_CONTINUE;
}


static void
mbs_start_clock()
{
	struct timespec tv;

	mbs_welcomescreen(0, NULL);

	tv.tv_sec = 2;
	tv.tv_nsec = 0;
	clock_timer_id = mbt_register(&tv, MB_TIMER_TYPE_AUTORELOAD, mbs_welcomescreen, NULL);
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
			assert(progress != NULL);
			DEBUG_PRINT("shell", "Destroying progress bar");
			mbv_window_destroy(progress);
			progress = NULL;
		} else if (last_status == MB_PLAYER_STATUS_PAUSED && status != MB_PLAYER_STATUS_PAUSED) {
			mb_player_showoverlaytext(player, "", 1,
				MBV_ALIGN_LEFT);
		}

		if (clock_timer_id != 0 && status != MB_PLAYER_STATUS_READY) {
			mbt_cancel(clock_timer_id);
		}

		switch (status) {
		case MB_PLAYER_STATUS_READY:
			DEBUG_PRINT("shell", "Player state changed to READY");
			/* mbs_welcomescreen(); */
			mbs_start_clock();
			break;

		case MB_PLAYER_STATUS_BUFFERING:
			if (last_status != MB_PLAYER_STATUS_BUFFERING) {
				DEBUG_PRINT("shell", "Player state changed to BUFFERING");
			}

			if (progress == NULL) {
				int sw, sh, px, py;

				DEBUG_PRINT("shell", "Initializing progress bar");

				mbs_clearscreen();
				mbv_window_getsize(root_window, &sw, &sh);

				pw = (sw * 70) / 100;
				ph = 30;
				px = (sw / 2) - (pw / 2);
				py = (sh / 2) - (ph / 2);

				progress = mbv_window_new(NULL, px, py, pw, ph);
				assert(progress != NULL);

				mbv_window_show(progress);

			} else {
				int donewidth = (pw * mb_player_bufferstate(inst)) / 100;

				assert(progress != NULL);

				mbv_window_clear(progress, MBV_DEFAULT_BACKGROUND);
				mbv_window_fillrectangle(progress, 0, 0, donewidth, ph);
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

	/* initialize main media player */
	player = mb_player_new(NULL);
	if (player == NULL) {
		fprintf(stderr, "Could not initialize main media player\n");
		return -1;
	}

	/* register for status updates */
	mb_player_add_status_callback(player, mbs_playerstatuschanged);

	return 0;
}


int
mbs_show_dialog(void)
{
	int quit = 0;
	mbi_event e;

	/* mbs_welcomescreen(); */
	mbs_start_clock();

	/* grab the input device */
	if ((input_fd = mbi_grab_input()) == -1) {
		fprintf(stderr, "mbs_show() -- mbi_grab_input failed\n");
		return -1;
	}

	/* run the message loop */
	while (!quit && read_or_eof(input_fd, &e, sizeof(mbi_event)) != 0) {
		switch (e) {
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
		}
		default:
			fprintf(stderr, "mbs: Received event %i\n", (int) e);
			break;
		}
		mb_player_update(player);
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

