/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef __MB_VIDEO_H__
#define __MB_VIDEO_H__
#ifdef HAVE_CONFIG_H
#	include "../config.h"
#endif
#include <stdint.h>
#include <pango/pangocairo.h>
#include "../dispatch.h"
#include "../delegate.h"
#include "video-drv.h"
#include "video-software.h"

#ifdef ENABLE_DIRECTFB
#	include "video-directfb.h"
#endif
#ifdef ENABLE_VC4
#	include "video-vc4.h"
#endif
#ifdef ENABLE_LIBDRM
#	include "video-drm.h"
#endif
#ifdef ENABLE_X11
#	include "video-x11.h"
#endif



/**
 * Window flags */
#define AVBOX_WNDFLAGS_NONE		(0x0)
#define AVBOX_WNDFLAGS_INPUT		(0x1)
#define AVBOX_WNDFLAGS_SUBWINDOW	(0x2)
#define AVBOX_WNDFLAGS_DECORATED	(0x4)
#define AVBOX_WNDFLAGS_ALPHABLEND	(0x8)

#define AVBOX_COLOR_R(x) (((x) >> 24) & 0xFF)
#define AVBOX_COLOR_G(x) (((x) >> 16) & 0xFF)
#define AVBOX_COLOR_B(x) (((x) >>  8) & 0xFF)
#define AVBOX_COLOR_A(x) ((x) & 0xFF)

#define AVBOX_COLOR_PREMULT(color) \
	(((color) & 0xFF000000) | \
	(((((color) >> 24) * (((color) >> 16) & 0xFF)) >> 8) << 16) | \
	(((((color) >> 24) * (((color) >>  8) & 0xFF)) >> 8) <<  8) | \
	(((((color) >> 24) * (((color) >>  0) & 0xFF)) >> 8) <<  0))

#define AVBOX_COLOR(color) \
	(AVBOX_COLOR_A((color)) << 24 | \
	AVBOX_COLOR_R((color)) << 16 | \
	AVBOX_COLOR_G((color)) <<  8 | \
	AVBOX_COLOR_B((color)))


#define MBV_DEFAULT_FONT        ("/usr/share/fonts/dejavu/DejaVuSansCondensed-Bold.ttf")
#define MBV_DEFAULT_FOREGROUND  AVBOX_COLOR(0xFFFFFFFF)
#define MBV_DEFAULT_BACKGROUND  AVBOX_COLOR(0x0951AFBF)
#define MBV_DEFAULT_OPACITY     (100)

/* convenience macros for converting colors to RGBA floating point
 * for cairo */
#define CAIRO_COLOR_RGBA_A(x) (((double)((x >> 24) & 0xFF)) / 255.0)
#define CAIRO_COLOR_RGBA_R(x) (((double)((x >> 16) & 0xFF)) / 255.0)
#define CAIRO_COLOR_RGBA_G(x) (((double)((x >>  8) & 0xFF)) / 255.0)
#define CAIRO_COLOR_RGBA_B(x) (((double)((x      ) & 0xFF)) / 255.0)
#define CAIRO_COLOR_RGBA(color) \
	CAIRO_COLOR_RGBA_R(color), \
	CAIRO_COLOR_RGBA_G(color), \
	CAIRO_COLOR_RGBA_B(color), \
	CAIRO_COLOR_RGBA_A(color)


struct avbox_window;
struct mbv_font;


enum avbox_pixel_format
{
	AVBOX_PIXFMT_UNKNOWN = 0,
	AVBOX_PIXFMT_BGRA = 1,
	AVBOX_PIXFMT_YUV420P = 2,
	AVBOX_PIXFMT_MMAL = 3
};


/**
 * Function that handles window painting.
 */
typedef int (*avbox_video_draw_fn)(
	struct avbox_window * const window, void * context);

/**
 * Represents a rectangle.
 */
struct avbox_rect
{
	int x;
	int y;
	int w;
	int h;
};


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


/**
 * Delegate a function call to the main thread under the
 * window's context.
 */
struct avbox_delegate *
avbox_window_delegate(struct avbox_window * const window,
	avbox_delegate_fn func, void *arg);


cairo_t *
avbox_window_cairo_begin(struct avbox_window *window);


PangoFontDescription *
mbv_getdefaultfont(void);


void
avbox_window_cairo_end(struct avbox_window *window);


/**
 * Lock the window surface, map it, and return a pointer
 * and a pitch describing the content window surface.
 */
uint8_t *
avbox_window_lock(struct avbox_window * const window, int flags, int *pitch);


/**
 * Unlock and unmap the content window surface.
 */
void
avbox_window_unlock(struct avbox_window * const window);


/**
 * Get the window's dirty bit
 */
int
avbox_window_dirty(const struct avbox_window * const window);

/**
 * Set the window's dirty bit.
 */
void
avbox_window_setdirty(struct avbox_window * const window, int value);


int
avbox_window_blit(struct avbox_window * const dest,
	struct avbox_window * const src, int flags, int x, int y);


int
avbox_window_scaleblit(
	struct avbox_window * const dst,
	struct avbox_window * const src,
	int flags, const int x, const int y, const int w, const int h);


/**
 * Gets the window's foreground color
 */
uint32_t
avbox_window_getcolor(const struct avbox_window *window);


/**
 * Gets the window's background color.
 */
uint32_t
avbox_window_getbackground(const struct avbox_window *window);


/**
 * avbox_window_isvisible() -- Checks if the given window is visible.
 */
int
avbox_window_isvisible(struct avbox_window *window);


/**
 * Move window to front.
 */
void
avbox_window_tofront(struct avbox_window *window);


int
avbox_window_getsize(struct avbox_window *window, int *width, int *height);


void
avbox_window_fillrectangle(struct avbox_window *window, int x, int y, int w, int h);


/**
 * Draw a round rectangle
 */
int
avbox_window_roundrectangle(struct avbox_window * window, struct avbox_rect *rect, int border_width, int rad);


int
avbox_window_settitle(struct avbox_window *window, const char *title);


/**
 * Gets the window's dispatch object.
 */
struct avbox_object*
avbox_window_object(struct avbox_window * const window);


int
mbv_getdefaultfontheight(void);


struct mbv_font *
mbv_font_new(char *file, int height);


void
mbv_font_destroy(struct mbv_font *inst);


/**
 * Returns 1 if the window has been damaged and needs
 * to be fully repaint. This is useful for the root window
 * that may be damaged by other windows rendering to it during
 * full screen updates.
 */
int
avbox_window_damaged(struct avbox_window * const window);


int
avbox_window_blitbuf(
	struct avbox_window *window,
	unsigned int pix_fmt, void **buf, int *pitch, int width, int height,
	int x, int y);


struct avbox_window*
avbox_window_new(
	struct avbox_window *parent,
	const char * const identifier,
	int flags,
	const int x, const int y, int w, int h,
	avbox_message_handler msghandler,
	avbox_video_draw_fn paint, void *context);


/**
 * Sets the drawing function.
 */
void
avbox_window_setdrawfunc(struct avbox_window * const window,
	avbox_video_draw_fn func, void * const context);


void
avbox_window_update(struct avbox_window *window);


struct avbox_window*
avbox_video_getrootwindow(int screen);


/**
 * avbox_window_clear() -- Clear the window surface
 */
void
avbox_window_clear(struct avbox_window * const window);


/**
 * Show the window.
 */
void
avbox_window_show(struct avbox_window * const window);


void
avbox_window_hide(struct avbox_window *win);


void
avbox_window_getcanvassize(const struct avbox_window * const window,
	int * const width, int * const height);


void
avbox_window_setcolor(struct avbox_window *window, uint32_t color);


/**
 * Sets the window's background color.
 */
void
avbox_window_setbgcolor(struct avbox_window * const window,
	const uint32_t color);


void
avbox_window_drawline(struct avbox_window *window,
	int x1, int y1, int x2, int y2);


void
avbox_window_drawstring(struct avbox_window *window,
	char *str, int x, int y);


void
avbox_window_destroy(struct avbox_window *win);


int
avbox_video_init(int argc, char **argv);


void
avbox_video_shutdown(void);

#endif
