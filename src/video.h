#ifndef __MB_VIDEO_H__
#define __MB_VIDEO_H__

#include <stdint.h>
#include <pango/pangocairo.h>


#define MBV_DEFAULT_FONT        ("/usr/share/fonts/dejavu/DejaVuSansCondensed-Bold.ttf")
#define MBV_DEFAULT_FOREGROUND  (0xFFFFFFFF)
#define MBV_DEFAULT_BACKGROUND  (0x0951AFFF)
#define MBV_DEFAULT_OPACITY     (100)


/* convenience macros for converting colors to RGBA floating point
 * for cairo */
#define CAIRO_COLOR_RGBA_R(x) (((double)((x >> 24) & 0xFF)) / 256.0F)
#define CAIRO_COLOR_RGBA_G(x) (((double)((x >> 16) & 0xFF)) / 256.0F)
#define CAIRO_COLOR_RGBA_B(x) (((double)((x >>  8) & 0xFF)) / 256.0F)
#define CAIRO_COLOR_RGBA_A(x) (((double)((x      ) & 0xFF)) / 256.0F)
#define CAIRO_COLOR_RGBA(color) \
	CAIRO_COLOR_RGBA_R(color), \
	CAIRO_COLOR_RGBA_G(color), \
	CAIRO_COLOR_RGBA_B(color), \
	CAIRO_COLOR_RGBA_A(color)


struct mbv_window;
struct mbv_font;

enum mbv_alignment
{
	MBV_ALIGN_LEFT = 1,
	MBV_ALIGN_CENTER = 2,
	MBV_ALIGN_RIGHT = 4
};

static inline PangoAlignment
mbv_get_pango_alignment(enum mbv_alignment alignment)
{
	switch (alignment) {
	case MBV_ALIGN_LEFT: return PANGO_ALIGN_LEFT;
	case MBV_ALIGN_CENTER: return PANGO_ALIGN_CENTER;
	case MBV_ALIGN_RIGHT: return PANGO_ALIGN_RIGHT;
	default: return PANGO_ALIGN_CENTER;
	}
}


cairo_t *
mbv_window_cairo_begin(struct mbv_window *window);


PangoFontDescription *
mbv_getdefaultfont(void);


void
mbv_window_cairo_end(struct mbv_window *window);


void
mbv_getscreensize(int *width, int *height);


int
mbv_window_getsize(struct mbv_window *window, int *width, int *height);


void
mbv_window_fillrectangle(struct mbv_window *window, int x, int y, int w, int h);


int
mbv_window_settitle(struct mbv_window *window, char *title);


int
mbv_isfbdev(void);


int
mbv_getdefaultfontheight(void);


struct mbv_font *
mbv_font_new(char *file, int height);


void
mbv_font_destroy(struct mbv_font *inst);


int
mbv_window_blit_buffer(
	struct mbv_window *window, void *buf, int width, int height,
	int x, int y);


struct mbv_window*
mbv_window_new(char *title,
	int x, 	int y, int width, int height);


struct mbv_window*
mbv_window_getchildwindow(struct mbv_window *window,
	int x, int y, int width, int height);


void
mbv_window_update(struct mbv_window *window);


struct mbv_window*
mbv_getrootwindow(void);


/**
 * mbv_window_clear() -- Clear the window surface
 */
void
mbv_window_clear(struct mbv_window *win, uint32_t color);


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


void
mbv_init(int argc, char **argv);


void
mbv_destroy();

#endif
