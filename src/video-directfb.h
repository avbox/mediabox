#ifndef __MB_VIDEO_DIRECTFB__
#define __MB_VIDEO_DIRECTFB__

#include <stdint.h>
#include <cairo/cairo.h>

struct mbv_dfb_window;


int
mbv_dfb_isfbdev(void);


/**
 * mbv_dfb_window_cairo_begin() -- Gets a cairo context for drawing
 * to the window
 */
cairo_t *
mbv_dfb_window_cairo_begin(struct mbv_dfb_window *window);


/**
 * mbv_dfb_window_cairo_end() -- Ends a cairo drawing session and
 * unlocks the surface
 */
void
mbv_dfb_window_cairo_end(struct mbv_dfb_window *window);


void
mbv_dfb_getscreensize(int *width, int *height);


int
mbv_dfb_window_isvisible(struct mbv_dfb_window *window);


void
mbv_dfb_window_update(struct mbv_dfb_window *window);


int
mbv_dfb_window_blit_buffer(
	struct mbv_dfb_window *window, void *buf, int width, int height,
	int x, int y);


struct mbv_dfb_window*
mbv_dfb_window_new(
        char *title,
        int x,
        int y,
        int width,
        int height);


struct mbv_dfb_window*
mbv_dfb_window_getchildwindow(struct mbv_dfb_window *window,
	int x, int y, int width, int height);


void
mbv_dfb_window_show(struct mbv_dfb_window *win);

void
mbv_dfb_window_hide(struct mbv_dfb_window *win);

void
mbv_dfb_window_destroy(struct mbv_dfb_window *win);


struct mbv_dfb_window*
mbv_dfb_getrootwindow(void);


/**
 * mbv_init() -- Initialize video device
 */
struct mbv_dfb_window *
mbv_dfb_init(int argc, char **argv);

void
mbv_dfb_destroy();

#endif
