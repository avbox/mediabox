#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "video.h"
#include "input.h"
#include "player.h"

static struct mbv_window *window = NULL;

/**
 * mbm_init() -- Initialize the MediaBox menu
 */
int
mb_mainmenu_init(void)
{
	window = mbv_window_new("Menu",
		(mbv_screen_width_get() / 2) - 150,
		(mbv_screen_height_get() / 2) - 150,
		300,
		300);
	return 0;
}

int
mb_mainmenu_showdialog(void)
{
	int fd, quit = 0;
	mbi_event e;

	/* show the menu window */
        mbv_window_show(window);

	/* grab the input device */
	if ((fd = mbi_grab_input()) == -1) {
		fprintf(stderr, "mbs_show() -- mbi_grab_input failed\n");
		return -1;
	}

	/* run the message loop */
	while (!quit && read_or_eof(fd, &e, sizeof(mbi_event)) != 0) {
		switch (e) {
		case MBI_EVENT_BACK: 
			fprintf(stderr, "mb_mainmenu: Back button pressed\n");
			close(fd);
			quit = 1;
			break;
		case MBI_EVENT_PLAY:
			fprintf(stderr, "mb_mainmenu: Play button pressed\n");
			break;
		case MBI_EVENT_MENU:
			fprintf(stderr, "mb_mainmenu: Menu button pressed\n");
			break;
		default:
			fprintf(stderr, "mb_mainmenu: Received event %i\n", (int) e);
		}
	}

	mbv_window_destroy(window);

	return 0;
}

void
mb_mainmenu_destroy(void)
{
	mbv_window_destroy(window);
}

