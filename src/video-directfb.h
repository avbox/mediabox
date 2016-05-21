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
mbv_dfb_screen_width_get(void);

int
mbv_dfb_screen_height_get(void);

void
mbv_dfb_window_clear(struct mbv_window *win,
	unsigned char r, unsigned char g, unsigned char b, unsigned char a);


struct mbv_window*
mbv_dfb_window_new(
        char *title,
        int x,
        int y,
        int width,
        int height);


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


/**
 * mbv_dfb_window_drawline() -- Draw a line on a window
 */
void
mbv_dfb_window_drawline(struct mbv_window *window,
        int x1, int y1, int x2, int y2);


/**
 * mbv_init() -- Initialize video device
 */
void
mbv_dfb_init(int argc, char **argv);

void
mbv_dfb_destroy();

#endif
