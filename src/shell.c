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



#define MEDIA_FILE "/mov.mp4"

static struct mbv_window *root_window = NULL;
static struct mbp *player = NULL;


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
	player = mbp_init();
	if (player == NULL) {
		fprintf(stderr, "Could not initialize main media player\n");
		mbv_window_destroy(root_window);
		return -1;
	}

	return 0;
}


int
mbs_show_dialog(void)
{
	int fd, quit = 0;
	mbi_event e;

	/* show the root window */
	mbv_window_clear(root_window, 0x00, 0x00, 0x00, 0x00);
	mbv_window_setcolor(root_window, 0x8080ffff);
	mbv_window_drawline(root_window, 0, mbv_screen_height_get() / 2,
		mbv_screen_width_get() - 1, mbv_screen_height_get() / 2);
        mbv_window_show(root_window);

	/* grab the input device */
	if ((fd = mbi_grab_input()) == -1) {
		fprintf(stderr, "mbs_show() -- mbi_grab_input failed\n");
		return -1;
	}

	/* run the message loop */
	while (!quit && read_or_eof(fd, &e, sizeof(mbi_event)) != 0) {
		switch (e) {
		case MBI_EVENT_QUIT:
		{
			close(fd);
			quit = 1;
			break;
		}
		case MBI_EVENT_MENU:
		{
			fprintf(stderr, "Menu pressed\n");
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
		case MBI_EVENT_PLAY:
		{
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
			break;
		}
		case MBI_EVENT_STOP:
		{
			enum mb_player_status status;
			status = mb_player_getstatus(player);
			if (status != MB_PLAYER_STATUS_READY) {
				if (mbp_stop(player) == -1) {
					fprintf(stderr, "mbs: mbp_stop() failed\n");
				}
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
mbs_destroy(void)
{
	mbp_destroy(player);
	mbv_window_destroy(root_window);
}

