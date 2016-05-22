#ifndef __MB_VIDEO_H__
#define __MB_VIDEO_H__

#include <stdint.h>

struct mbv_window;

struct mbv_window*
mbv_window_new(
	char *title,
	int x,
	int y,
	int width,
	int height);


struct mbv_window*
mbv_window_getchildwindow(struct mbv_window *window,
	int x, int y, int width, int height);


/**
 * mbv_window_clear() -- Clear the window surface
 */
void
mbv_window_clear(struct mbv_window *win,
	unsigned char r, unsigned char g, unsigned char b, unsigned char a);


void
mbv_window_show(struct mbv_window *win);

void
mbv_window_hide(struct mbv_window *win);


void
mbv_window_getcanvassize(struct mbv_window *window,
	int *width, int *height);


void
mbv_window_setcolor(struct mbv_window *window, uint32_t color);

void
mbv_window_drawline(struct mbv_window *window,
	int x1, int y1, int x2, int y2);


void
mbv_window_drawstring(struct mbv_window *window,
	char *str, int x, int y);


void
mbv_window_destroy(struct mbv_window *win);

void
mbv_clear(void);

int
mbv_screen_width_get(void);

int
mbv_screen_height_get(void);

void
mbv_init(int argc, char **argv);

void
mbv_destroy();

#endif
