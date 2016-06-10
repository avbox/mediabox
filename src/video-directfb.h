#ifndef __MB_VIDEO_DIRECTFB__
#define __MB_VIDEO_DIRECTFB__

#include <stdint.h>

struct mbv_window;


/**
 * mb_video_clear()
 */
void
mbv_dfb_clear(void);


int
mbv_dfb_isfbdev(void);


int
mbv_dfb_window_settitle(struct mbv_window *window, char *title);


struct mbv_font *
mbv_dfb_font_new(char *file, int height);


void
mbv_dfb_font_destroy(struct mbv_font *inst);


void
mbv_dfb_window_fillrectangle(struct mbv_window *window, int x, int y, int w, int h);


void
mbv_dfb_getscreensize(int *width, int *height);


int
mbv_dfb_getdefaultfontheight(void);


int
mbv_dfb_screen_width_get(void);


int
mbv_dfb_screen_height_get(void);


void
mbv_dfb_window_clear(struct mbv_window *win, uint32_t color);


void
mbv_dfb_window_update(struct mbv_window *window);


int
mbv_dfb_window_getsize(struct mbv_window *window, int *width, int *height);


int
mbv_dfb_window_blit_buffer(
	struct mbv_window *window, void *buf, int width, int height,
	int x, int y);


struct mbv_window*
mbv_dfb_window_new(
        char *title,
        int x,
        int y,
        int width,
        int height);


struct mbv_window*
mbv_dfb_window_getchildwindow(struct mbv_window *window,
	int x, int y, int width, int height);


void
mbv_dfb_window_show(struct mbv_window *win);

void
mbv_dfb_window_hide(struct mbv_window *win);

void
mbv_dfb_window_destroy(struct mbv_window *win);

/**
 * mbv_dfb_window_setcolor() -- Set color for future operations
 */
void
mbv_dfb_window_setcolor(struct mbv_window *window, uint32_t color);


void
mbv_dfb_window_getcanvassize(struct mbv_window *window,
	int *width, int *height);


struct mbv_window*
mbv_dfb_getrootwindow(void);


/**
 * mbv_dfb_window_drawline() -- Draw a line on a window
 */
void
mbv_dfb_window_drawline(struct mbv_window *window,
        int x1, int y1, int x2, int y2);


void
mbv_dfb_window_drawstring(struct mbv_window *window,
	char *str, int x, int y);


/**
 * mbv_init() -- Initialize video device
 */
void
mbv_dfb_init(int argc, char **argv);

void
mbv_dfb_destroy();

#endif
