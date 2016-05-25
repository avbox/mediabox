#include <stdlib.h>

#include "video-directfb.h"

struct mbv_window;


int
mbv_window_getsize(struct mbv_window *window, int *width, int *height)
{
	return mbv_dfb_window_getsize(window, width, height);
}


int
mbv_window_blit_buffer(
	struct mbv_window *window, void *buf, int width, int height)
{
	return mbv_dfb_window_blit_buffer(window, buf, width, height);
}


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


struct mbv_window*
mbv_window_getchildwindow(struct mbv_window *window,
	int x, int y, int width, int height)
{
	return mbv_dfb_window_getchildwindow(window, x, y, width, height);
}


struct mbv_window*
mbv_getrootwindow(void)
{
	return mbv_dfb_getrootwindow();
}

void
mbv_window_clear(struct mbv_window *win,
	unsigned char r,
	unsigned char g,
	unsigned char b,
	unsigned char a)
{
	mbv_dfb_window_clear(win, r, g, b, a);
}


void
mbv_window_getcanvassize(struct mbv_window *window,
	int *width, int *height)
{
	mbv_dfb_window_getcanvassize(window, width, height);
}


void
mbv_window_setcolor(struct mbv_window *window, uint32_t color)
{
	mbv_dfb_window_setcolor(window, color);
}


void
mbv_window_drawline(struct mbv_window *window,
	int x1, int y1, int x2, int y2)
{
	mbv_dfb_window_drawline(window, x1, y1, x2, y2);
}


void
mbv_window_drawstring(struct mbv_window *window,
	char *str, int x, int y)
{
	mbv_dfb_window_drawstring(window, str, x, y);
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

