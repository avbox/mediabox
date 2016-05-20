#include <stdlib.h>

#include "video-directfb.h"

struct mbv_window*
mbv_window_new(
	char *title,
	int x,
	int y,
	int width,
	int height)
{
	return mbv_dfb_window_new(title, x, y, width, height);
}

void
mbv_window_show(struct mbv_window *win)
{
	mbv_dfb_window_show(win);
}

void
mbv_window_hide(struct mbv_window *win)
{
	mbv_dfb_window_hide(win);
}

void
mbv_window_destroy(struct mbv_window *win)
{
	mbv_dfb_window_destroy(win);
}

void
mbv_clear(void)
{
	mbv_dfb_clear();
}

int
mbv_screen_height_get(void)
{
	return mbv_dfb_screen_height_get();
}

int
mbv_screen_width_get(void)
{
	return mbv_dfb_screen_width_get();
}

void
mbv_init(int argc, char **argv)
{
	mbv_dfb_init(argc, argv);
}

void
mbv_destroy()
{
	mbv_dfb_destroy();
}

