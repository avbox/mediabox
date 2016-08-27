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
#include "debug.h"


#define MEDIA_FILE "/mov.mp4"


static int pw = 0, ph = 0;
static struct mbv_window *root_window = NULL;
static struct mbv_window *status_overlay = NULL;
static struct mbv_window *progress = NULL;
static struct mbp *player = NULL;
static int input_fd = -1;


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


static void
mbs_welcomescreen(void)
{
	/* show the root window */
	mbv_window_clear(root_window, 0x000000ff);
	mbv_window_setcolor(root_window, 0x8080ffff);
	mbv_window_drawline(root_window, 0, mbv_screen_height_get() / 2,
		mbv_screen_width_get() - 1, mbv_screen_height_get() / 2);
        mbv_window_show(root_window);
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
			/* destroy "PAUSED" window */
			assert(status_overlay != NULL);
			DEBUG_PRINT("shell", "Destroying status overlay");
			mbv_window_destroy(status_overlay);
		}

		switch (status) {
		case MB_PLAYER_STATUS_READY:
			DEBUG_PRINT("shell", "Player state changed to READY");
			mbs_welcomescreen();
			break;

		case MB_PLAYER_STATUS_BUFFERING:
			DEBUG_PRINT("shell", "Player state changed to BUFFERING");

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

				mbv_window_clear(progress, 0x3349ffFF);
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

			status_overlay = mbv_window_new(NULL, 25, 25, 200, 60);
			mbv_window_clear(status_overlay, 0x000000ff);
			mbv_window_setcolor(status_overlay, 0xffffffff);
			mbv_window_drawstring(status_overlay, "PAUSED", 100, 5);
			assert(status_overlay != NULL);
			mbv_window_show(status_overlay);
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

	mbs_welcomescreen();

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
		default:
			fprintf(stderr, "mbs: Received event %i\n", (int) e);
			break;
		}
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

