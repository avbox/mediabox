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

static struct mbv_window *root_window = NULL;
static struct mbp *player = NULL;

/**
 * mbs_init() -- Initialize the MediaBox shell
 */
int
mbs_init(void)
{
        root_window = mbv_window_new(
		NULL,
                0,
                0,
                mbv_screen_width_get(),
                mbv_screen_height_get());
	player = NULL;
	return 0;
}

int
mbs_show_dialog(void)
{
	struct mbv_window *menu_win;
	int fd, quit = 0, menu_visible = 0;
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
		case MBI_EVENT_BACK: 
			fprintf(stderr, "mbs: Back button pressed\n");
			close(fd);
			quit = 1;
			break;
		case MBI_EVENT_MENU:
			fprintf(stderr, "mbs: Play button pressed\n");
			if (mb_mainmenu_init() == -1) {
				fprintf(stderr, "Could not initialize main menu\n");
				break;
			}
			mb_mainmenu_showdialog();
			mb_mainmenu_destroy();
			fprintf(stderr, "mbs: Dialog dismissed\n");
			break;
		case MBI_EVENT_PLAY:
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
		default:
			fprintf(stderr, "mbs: Received event %i\n", (int) e);
		}
	}

	fprintf(stderr, "mbs: Exiting\n");
	return 0;
}

void
mbs_destroy(void)
{
	mbv_window_destroy(root_window);
}

