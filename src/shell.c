#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "video.h"
#include "input.h"
#include "mainmenu.h"
#include "player.h"
#include "su.h"


#define MEDIA_FILE "/mov.mp4"


static struct mbv_window *root_window = NULL;
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
	/* show the root window */
	mbv_window_clear(root_window, 0x00000000);
	mbv_window_setcolor(root_window, 0x8080ffff);
	mbv_window_drawline(root_window, 0, mbv_screen_height_get() / 2,
		mbv_screen_width_get() - 1, mbv_screen_height_get() / 2);
        mbv_window_show(root_window);
}


/**
 * mbs_playerstatuschanged() -- Handle player state change events
 */
static void
mbs_playerstatuschanged(struct mbp *inst, enum mb_player_status status)
{
	if (inst == player) {
		switch (status) {
		case MB_PLAYER_STATUS_READY:
			mbs_clearscreen();
			break;
		case MB_PLAYER_STATUS_PLAYING:
		case MB_PLAYER_STATUS_PAUSED:
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

	mbs_clearscreen();

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
				mbp_play(player, MEDIA_FILE);
				break;
			case MB_PLAYER_STATUS_PLAYING:
				mbp_pause(player);
				break;
			case MB_PLAYER_STATUS_PAUSED:
				mbp_play(player, NULL);
				break;
			}
			#if 0
			enum mb_player_status status;
			status = mb_player_getstatus(player);
			if (status == MB_PLAYER_STATUS_PAUSED) {
				fprintf(stderr, "mbs: Resuming\n");
				mbp_play(player, NULL);
			} else if (status == MB_PLAYER_STATUS_PLAYING) {
				fprintf(stderr, "mbs: Pausing\n");
				mbp_pause(player);
			} else if (status == MB_PLAYER_STATUS_READY) {
				fprintf(stderr, "mbs: Playing\n");
				mbp_play(player, MEDIA_FILE);
			} else {
				fprintf(stderr, "Status %i\n", status);
			}
			#endif
			break;
		}
		case MBI_EVENT_KBD_S:
		case MBI_EVENT_STOP:
		{
			if (mb_player_getstatus(player) != MB_PLAYER_STATUS_READY) {
				(void) mbp_stop(player);
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

