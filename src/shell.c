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


#define MEDIA_FILE "/media/InternalStorage/Movies/Avatar ECE (2009)/Avatar.ECE.2009.720p.BrRip.x264.bitloks.YIFY.mp4"


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
			enum mb_player_status status;
			status = mb_player_getstatus(player);

			/* pause the media player first */
			if (status == MB_PLAYER_STATUS_PLAYING) {
				(void) mbp_pause(player);
			}

			if (mb_mainmenu_init() == -1) {
				fprintf(stderr, "Could not initialize main menu\n");
				break;
			}
			if (mb_mainmenu_showdialog() == -1) {
				fprintf(stderr, "mbs: Main Menu dismissed\n");
			}
			mb_mainmenu_destroy();

			/* if we were playing resume it */
			if (status == MB_PLAYER_STATUS_PLAYING) {
				mbp_play(player, NULL);
			}

			
			mb_player_update(player); /* force player to redraw */

			break;
		}
		case MBI_EVENT_PLAY:
		{
			enum mb_player_status status;
			status = mb_player_getstatus(player);
			if (status == MB_PLAYER_STATUS_PAUSED) {
				mbp_play(player, NULL);
			} else if (status == MB_PLAYER_STATUS_PLAYING) {
				mbp_pause(player);
			} else if (status == MB_PLAYER_STATUS_READY) {
				mbp_play(player, MEDIA_FILE);
			} else {
				fprintf(stderr, "Status %i\n", status);
			}

			#if 0
			struct mbv_window *menu_win;
			static int menu_visible = 0;
			fprintf(stderr, "mbs: Play button pressed\n");
			if (!menu_visible) {
				menu_win = mbv_window_new("Hello World",
					(mbv_screen_width_get() / 2) - 150,
					(mbv_screen_height_get() / 2) - 150,
					300,
					300);
				mbv_window_show(menu_win);
			} else {
				mbv_window_destroy(menu_win);
			}
			menu_visible ^= 1;
			break;
			#endif
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

