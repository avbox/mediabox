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


#ifdef HAVE_CONFIG_H
#	include "../../config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pango/pangocairo.h>

#include "../ffmpeg_util.h"

#define LOG_MODULE "video"

#include "video.h"
#include "video-drv.h"
#include "input.h"
#include "../debug.h"
#include "../linkedlist.h"
#include "../log.h"
#include "../dispatch.h"
#include "../delegate.h"


#define FORCE_FULL_SCREEN_REPAINTS 1

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

#define ALIGNED(addr, bytes) \
    (((uintptr_t)(const void *)(addr)) % (bytes) == 0)


#define FONT_PADDING 	(3)


struct avbox_window;

LISTABLE_STRUCT(avbox_window_node,
	struct avbox_window *window;
);

/**
 * Represents a window object.
 */
struct avbox_window
{
	struct mbv_surface *surface;
	struct avbox_object *object;
	struct avbox_window *content_window;
	struct avbox_window *parent;
	struct avbox_window_node *node;
	struct avbox_window_node stack_node;
	avbox_video_draw_fn paint;
	avbox_message_handler handler;
	cairo_t *cairo_context;
	const char *title;
	const char *identifier; /* used for debugging purposes */
	struct avbox_rect rect;
	int visible;
	int flags;
	int damaged;
	int decor_dirty;
	int dirty;
	uint32_t foreground_color;
	uint32_t background_color;
	void *user_context;
	void *draw_context;
	LIST_DECLARE(children);
};


static struct mbv_drv_funcs driver;
static struct avbox_window root_window;
static PangoFontDescription *font_desc;
static int default_font_height = 32;

LIST window_stack;


/**
 * Checks if inner is inside outter. Returns 1 if
 * it is, 0 otherwise
 */
#if 0
static int
avbox_rect_isinside(const struct avbox_rect * const inner,
	const struct avbox_rect * const outter)
{
	return inner->x >= outter->x && inner->y >= outter->y &&
		(inner->x + inner->w) <= (outter->x + outter->w) &&
		(inner->y + inner->h) <= (outter->y + outter->h);
}
#endif


/**
 * Check if two rectangles overlap.
 */
static int
avbox_rect_overlaps(const struct avbox_rect * const rect1,
	const struct avbox_rect * const rect2)
{
	return 1;
}


/**
 * Checks if rect1 covers rect2.
 */
static int
avbox_rect_covers(const struct avbox_rect * const rect1,
	const struct avbox_rect * const rect2)
{
	if (rect1->x <= rect2->x && rect1->y <= rect2->y) {
		if (rect1->w >= (rect2->w + (rect2->x - rect1->x)) &&
			rect1->h >= (rect2->h + (rect2->y - rect1->y))) {
			return 1;
		}
	}
	return 0;
}


/**
 * Gets the cairo context for a window. This is the
 * internal version that works directly on the surface
 * (and not the content window subsurface).
 */
static cairo_t *
__window_cairobegin(struct avbox_window * const window)
{
	cairo_surface_t *surface;
	int pitch;
	void *buf;

	ASSERT(window != NULL);

	if ((buf = driver.surface_lock(window->surface, MBV_LOCKFLAGS_READ | MBV_LOCKFLAGS_WRITE, &pitch)) == NULL) {
		LOG_PRINT_ERROR("Could not lock surface!!!");
		return NULL;
	}

	surface = cairo_image_surface_create_for_data(buf,
		CAIRO_FORMAT_ARGB32, window->rect.w, window->rect.h, pitch);
	if (surface == NULL) {
		driver.surface_unlock(window->surface);
		return NULL;
	}

	window->cairo_context = cairo_create(surface);
	if (window->cairo_context == NULL) {
		driver.surface_unlock(window->surface);
	} else {
		cairo_set_operator(window->cairo_context, CAIRO_OPERATOR_SOURCE);
	}

	cairo_surface_destroy(surface);

	return window->cairo_context;
}


/**
 * Releases the cairo context for the window
 */
static void
__window_cairoend(struct avbox_window *window)
{
	assert(window != NULL);
	assert(window->cairo_context != NULL);
	cairo_destroy(window->cairo_context);
	window->cairo_context = NULL;
	driver.surface_unlock(window->surface);
}


/**
 * Clear the window. This is the internal version
 * that works on the whole window.
 */
static void
__window_clear(struct avbox_window *window, const uint32_t color)
{
	int pitch;
	uint8_t *buf;
	unsigned int flags;

	if (window->title != NULL) {
		avbox_window_settitle(window, window->title);
	}

	flags = MBV_LOCKFLAGS_WRITE;

clear:
	if ((buf = avbox_window_lock(window, flags, &pitch)) == NULL) {
		LOG_VPRINT_ERROR("Could not lock window: %s",
			strerror(errno));
	} else {
		for (int stride = 0; stride < window->content_window->rect.h; buf += pitch, stride++) {
			int pix;
#if UINTPTR_MAX == 0xffffffffffffffff
			if (ALIGNED(buf, 8)) {
				uint64_t *pixel64 = (uint64_t*) buf;
				const uint64_t color64 = (uint64_t) color << 32 | color;
				for (pix = 0; pix < window->content_window->rect.w - 1; pix += 2) {
					*pixel64++ = color64;
				}
				if (pix < window->content_window->rect.w) {
					*((uint32_t*) pixel64) = color;
				}
			}
			else
#endif
			{
				uint32_t *pixel = (uint32_t*) buf;
				ASSERT(ALIGNED(buf, 4));
				for (pix = 0; pix < window->content_window->rect.w; pix++) {
					*pixel++ = color;
				}
			}
		}
		avbox_window_unlock(window);
	}

	/* if the window surface is double buffered we need to clear
	 * the front buffer as well */
	if (!(flags & MBV_LOCKFLAGS_FRONT) &&
		driver.surface_doublebuffered(window->content_window->surface)) {
		flags |= MBV_LOCKFLAGS_FRONT;
		goto clear;
	}
}


/**
 * Lock the window surface, map it, and return a pointer
 * and a pitch describing the content window surface.
 */
uint8_t *
avbox_window_lock(struct avbox_window * const window, int flags, int *pitch)
{
	return driver.surface_lock(window->content_window->surface, flags, pitch);
}


/**
 * Unlock and unmap the content window surface.
 */
void
avbox_window_unlock(struct avbox_window * const window)
{
	driver.surface_unlock(window->content_window->surface);
}


/**
 * Gets the cairo context for a window
 */
cairo_t *
avbox_window_cairo_begin(struct avbox_window * const window)
{
	return __window_cairobegin(window->content_window);
}


/**
 * Releases the cairo context for the window
 */
void avbox_window_cairo_end(struct avbox_window * const window)
{
	assert(window != NULL);
	__window_cairoend(window->content_window);
}

/**
 * Clear the window.
 */
void avbox_window_clear(struct avbox_window * const window)
{
	assert(window != NULL);
	__window_clear(window, AVBOX_COLOR_PREMULT(window->background_color));
}


#ifndef NDEBUG
static inline int
mbv_getfontsize(PangoFontDescription *desc)
{
	int sz;
	sz = pango_font_description_get_size(desc);

	if (!pango_font_description_get_size_is_absolute(desc)) {
		sz = (sz * 96) / (PANGO_SCALE * 72);
	}

	return sz;
}
#endif

/**
 * mbv_getdefaultfont() -- Gets the default system font description.
 */
PangoFontDescription *
mbv_getdefaultfont(void)
{
	return font_desc;
}


/**
 * Checks if the given window is visible.
 */
int
avbox_window_isvisible(struct avbox_window *window)
{
	return window->visible;
}


/**
 * Gets the window's dispatch object.
 */
struct avbox_object*
avbox_window_object(struct avbox_window * const window)
{
	assert(window != NULL);
	return window->object;
}


int
avbox_window_getsize(struct avbox_window *window, int *width, int *height)
{
	*width = window->rect.w;
	*height = window->rect.h;
	return 0;
}


/**
 * Sets the window title.
 */
int
avbox_window_settitle(struct avbox_window *window, const char *title)
{
	char *title_copy;

	assert(window->content_window != window); /* is a window WITH title */

	title_copy = strdup(title);
	if (title_copy == NULL) {
		assert(errno == ENOMEM);
		return -1;
	}

	if (window->title != NULL) {
		free((void*)window->title);
	}

	window->title = title_copy;
	return 0;
}


/**
 * Returns 1 if the window has been damaged and needs
 * to be fully repaint. This is useful for the root window
 * that may be damaged by other windows rendering to it during
 * full screen updates.
 */
int
avbox_window_damaged(struct avbox_window * const window)
{
	return window->damaged;
}


/**
 * Fills a rectangle inside a window.
 */
void
avbox_window_fillrectangle(struct avbox_window *window, int x, int y, int w, int h)
{
	cairo_t *context;

	if ((context = avbox_window_cairo_begin(window)) != NULL) {
		cairo_move_to(context, x, y);
		cairo_line_to(context, x + w, y);
		cairo_line_to(context, x + w, y + h);
		cairo_line_to(context, x, y + h);
		cairo_line_to(context, x, y);
		cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->foreground_color));
		cairo_fill(context);
		avbox_window_cairo_end(window);
	}
}


/**
 * Draw a round rectangle
 */
int
avbox_window_roundrectangle(struct avbox_window * window, struct avbox_rect *rect,
	int border_width, int rad)
{
	cairo_t *cr;

	/* a custom shape that could be wrapped in a function */
	double x, y, w, h, aspect, corner_radius, radius;

	double degrees = M_PI / 180.0;

	/* get cairo context */
	if ((cr = avbox_window_cairo_begin(window)) == NULL) {
		LOG_VPRINT_ERROR("Could not get cairo context: %s",
			strerror(errno));
		return -1;
	}

	x = (double) rect->x;
	y = (double) rect->y;
	w = (double) rect->w;
	h = (double) rect->h;

	aspect = 1.0;
	corner_radius = h / rad;
	radius = corner_radius / aspect;

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - radius, y + radius, radius, -90 * degrees, 0 * degrees);
	cairo_arc(cr, x + w - radius, y + h - radius, radius, 0 * degrees, 90 * degrees);
	cairo_arc(cr, x + radius, y + h - radius, radius, 90 * degrees, 180 * degrees);
	cairo_arc(cr, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
	cairo_close_path(cr);

	cairo_set_source_rgba(cr, CAIRO_COLOR_RGBA(window->background_color));
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, CAIRO_COLOR_RGBA(window->foreground_color));
	cairo_set_line_width(cr, (double) border_width);
	cairo_stroke(cr);

	avbox_window_cairo_end(window);

	return 0;
}

int
mbv_getdefaultfontheight(void)
{
	return default_font_height;
}


/**
 * Blits a buffer into a window's surface.
 */
int
avbox_window_blitbuf(
	struct avbox_window *window,
	unsigned int pix_fmt, void **buf, int *pitch, int width, int height,
	int x, int y)
{
	int ret;
	ret = driver.surface_blitbuf(
		window->content_window->surface,
		pix_fmt, buf, pitch, MBV_BLITFLAGS_NONE, width, height, x, y);
	return ret;
}


/**
 * Blit a window to another window
 */
int
avbox_window_blit(struct avbox_window * const dest,
	struct avbox_window * const src, int flags, int x, int y)
{
	return driver.surface_blit(dest->content_window->surface,
		src->content_window->surface, flags, x, y);
}


int
avbox_window_scaleblit(
	struct avbox_window * const dst,
	struct avbox_window * const src,
	int flags, const int x, const int y, const int w, const int h)
{
	if (driver.surface_scaleblit == NULL) {
		uint8_t *bufdst, *bufsrc;
		int dstpitch, srcpitch;
		struct SwsContext *swscale;

		if ((swscale = sws_getContext(
			src->content_window->rect.w,
			src->content_window->rect.h,
			MB_DECODER_PIX_FMT,
			w,
			h,
			MB_DECODER_PIX_FMT,
			SWS_FAST_BILINEAR,
			NULL, NULL, NULL)) == NULL) {
			LOG_PRINT_ERROR("Could not create swscale context!");
			return -1;
		}

		if ((bufdst = avbox_window_lock(dst, MBV_LOCKFLAGS_WRITE, &dstpitch)) == NULL) {
			sws_freeContext(swscale);
			return -1;
		}

		if ((bufsrc = avbox_window_lock(src, MBV_LOCKFLAGS_READ, &srcpitch)) == NULL) {
			avbox_window_unlock(dst);
			sws_freeContext(swscale);
			return -1;
		}

		bufdst += dstpitch * y;
		bufdst += x * 4;
		sws_scale(swscale, (uint8_t const * const *) &bufsrc,
			&srcpitch, 0, src->content_window->rect.h, &bufdst, &dstpitch);
		avbox_window_unlock(dst);
		avbox_window_unlock(src);
		sws_freeContext(swscale);
		return 0;
	} else {
		return driver.surface_scaleblit(
			dst->content_window->surface,
			src->content_window->surface,
			flags, x, y, w, h);
	}
}


static int
avbox_window_reallyvisible(struct avbox_window * const window)
{
	if (window->parent == &root_window) {
		struct avbox_window_node *damaging_window =
			LIST_NEXT(struct avbox_window_node*, &window->stack_node);

		/* for now we return 0 only when one or more windows cover
		 * the whole window. If the window is partially covered by several
		 * windows so that the entire window is cover it will still be painted.
		 * TODO: We need to do better */
		while (!LIST_ISNULL(&window_stack, damaging_window)) {
			if (avbox_rect_covers(&damaging_window->window->rect, &window->rect)) {
				return 0;
			}
			damaging_window = LIST_NEXT(struct avbox_window_node*,
				damaging_window);
		}
	}

	return 1;
}


/**
 * This is the internal repaint handler
 */
static int
avbox_window_paint(struct avbox_window * const window, int update)
{
	int blitflags = MBV_BLITFLAGS_NONE;
	struct avbox_window_node *damaged_window;
	struct avbox_window_node *child;

	/* DEBUG_VPRINT("video", "avbox_window_paint(\"%s\")",
		window->identifier); */

	if (!window->visible || !avbox_window_reallyvisible(window)) {
		return 0;
	}

	if (window->flags & AVBOX_WNDFLAGS_ALPHABLEND) {
		blitflags |= MBV_BLITFLAGS_ALPHABLEND;
	}

	/* if the window is dirty invoke the user defined
	 * paint handler */
	if (window->paint != NULL && window->dirty) {
		window->paint(window, window->draw_context);
	}

	/* invoke repaint handler for all subwindows */
	LIST_FOREACH(struct avbox_window_node *, child, &window->children) {
		avbox_window_paint(child->window, update);
	}

	/* blit window */
	driver.surface_update(window->surface, blitflags, update);

	/* if this is the root window we need to paint all
	 * visible windows */
	if (window->parent == &root_window) {
		damaged_window = LIST_NEXT(struct avbox_window_node*,
			&window->stack_node);
		while (!LIST_ISNULL(&window_stack, damaged_window)) {
			if (avbox_rect_overlaps(&window->rect, &damaged_window->window->rect)) {
				avbox_window_paint(damaged_window->window, update);
				if (avbox_rect_covers(&damaged_window->window->rect, &window->rect)) {
					break;
				}
				damaged_window = LIST_NEXT(struct avbox_window_node*,
					damaged_window);
			}
		}
	}
	return 0;
}


/**
 * Repaints the window decoration
 */
static int
avbox_window_paintdecor(struct avbox_window * const window, void * const ctx)
{
	cairo_t *context;
	PangoLayout *layout;

	ASSERT(window->content_window != window); /* is a window WITH title */

	if (window->decor_dirty) {
		if ((context = __window_cairobegin(window)) != NULL) {

			/* first clear the title window */
			cairo_move_to(context, 0, 0);
			cairo_line_to(context, window->rect.w, 0);
			cairo_line_to(context, window->rect.w, window->rect.h);
			cairo_line_to(context, 0, window->rect.h);
			cairo_line_to(context, 0, 0);
			cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(AVBOX_COLOR(0xcccccc00)));
			cairo_fill(context);

			/* a custom shape that could be wrapped in a function */
			const double degrees = M_PI / 180.0;
			const double x = 0;
			const double y = 0;
			const double w = (double) window->rect.w;
			const double h = (double) window->rect.h;
			const double corner_radius = 19.0;
			const double aspect = 1.0;
			const double radius = corner_radius / aspect;

			cairo_new_sub_path(context);
			cairo_arc(context, x + w - radius, y + radius, radius, -90 * degrees, 0 * degrees);
			cairo_arc(context, x + w - radius, y + h - radius, radius, 0 * degrees, 90 * degrees);
			cairo_arc(context, x + radius, y + h - radius, radius, 90 * degrees, 180 * degrees);
			cairo_arc(context, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
			cairo_close_path(context);

			cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->background_color));
			cairo_fill_preserve(context);
			cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->foreground_color));
			cairo_set_line_width(context, 2.0);
			cairo_stroke(context);

			if ((layout = pango_cairo_create_layout(context)) != NULL) {

				pango_layout_set_font_description(layout, font_desc);
				pango_layout_set_width(layout, window->rect.w * PANGO_SCALE);
				pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
				pango_layout_set_text(layout, window->title, -1);

				cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->foreground_color));
				cairo_move_to(context, 0, 0);
				pango_cairo_update_layout(context, layout);
				pango_cairo_show_layout(context, layout);

				/* free the layout */
				g_object_unref(layout);

				window->decor_dirty = 0;

			} else {
				DEBUG_PRINT("video", "Could not create layout");
			}

			__window_cairoend(window);
		} else {
			LOG_PRINT_ERROR("Could not create cairo context!");
		}
	}

	/* invoke the content window repaint handler */
	return avbox_window_paint(window->content_window, 1);
}


static void
__avbox_window_destroy(struct avbox_window * const window)
{
	ASSERT(window != NULL);
	ASSERT(window->surface != NULL);
	ASSERT(window->content_window != NULL);
	ASSERT(window != &root_window);

	/* if the window is visible hide it before
	 * destroying it */
	if (window->visible && window->parent == &root_window &&
		!(window->flags & AVBOX_WNDFLAGS_SUBWINDOW)) {
		avbox_window_hide(window);
	}

	LIST_REMOVE(window->node);
	free(window->node);

	if (window->title != NULL) {
		free((void*)window->title);
	}
	if (window->content_window != window) {
		avbox_window_destroy(window->content_window);
	}
}


static void
__avbox_window_cleanup(struct avbox_window * const window)
{
	if (window->identifier) {
		free((void*) window->identifier);
	}
	driver.surface_destroy(window->surface);
	free(window);
}


/**
 * Handle window messages.
 */
static int
avbox_window_handler(void *context, struct avbox_message * const msg)
{
	struct avbox_window * const window = context;
	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_DELEGATE:
	{
		struct avbox_delegate * const del =
			avbox_message_payload(msg);
		avbox_delegate_execute(del);
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		/* call the user defined destructor */
		if (window->handler != NULL) {
			int ret = window->handler(window->user_context, msg);
			if (ret == AVBOX_DISPATCH_OK) {
				__avbox_window_destroy(window);
			} else {
				return ret;
			}
		} else {
			__avbox_window_destroy(window);
		}
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
		/* call the user defined destructor */
		if (window->handler != NULL) {
			(void) window->handler(window->user_context, msg);
		}
		__avbox_window_cleanup(window);
		return AVBOX_DISPATCH_OK;
	default:
		if (window->handler != NULL) {
			return window->handler(window->user_context, msg);
		} else {
			DEBUG_VABORT("video", "Invalid message %d and no handler!!",
				avbox_message_id(msg));
		}
	}
}


/**
 * Delegate a function call to the main thread under the
 * window's context.
 */
struct avbox_delegate *
avbox_window_delegate(struct avbox_window * const window,
	avbox_delegate_fn func, void *arg)
{
	struct avbox_delegate *del;

	if (window->handler == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* create delegate */
	if ((del = avbox_delegate_new(func, arg, 0)) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	/* send delegate to the main thread */
	if (avbox_object_sendmsg(&window->object,
		AVBOX_MESSAGETYPE_DELEGATE, AVBOX_DISPATCH_UNICAST, del) == NULL) {
		LOG_VPRINT_ERROR("Could not delegate to window: %s",
			strerror(errno));
		avbox_delegate_destroy(del);
		return NULL;
	}

	return del;
}


/**
 * Gets a child window
 */
static struct avbox_window*
avbox_window_subwindow(struct avbox_window * const window,
	const char * const identifier,
	int flags,
	const int x, const int y, int w, int h,
	avbox_message_handler msghandler,
	avbox_video_draw_fn paint,
	void *user_context)
{
	struct avbox_window *new_window;
	struct avbox_window_node *window_node;

	/* DEBUG_VPRINT("video", "avbox_window_getchildwindow(\"%s\")",
		identifier); */

	/* allocate memory for the window and it's node */
	if ((new_window = malloc(sizeof(struct avbox_window))) == NULL) {
		fprintf(stderr, "video: Could not allocate window object. Out of memory\n");
		return NULL;
	}
	if ((window_node = malloc(sizeof(struct avbox_window_node))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate window node. Out of memory");
		free(new_window);
		return NULL;
	}

	/* if width or height is -1 adjust it to the
	 * size of the parent window */
	if (w == -1 || h == -1) {
		int pw, ph;
		avbox_window_getcanvassize(window, &pw, &ph);
		if (w == -1) {
			w = pw;
		}
		if (h == -1) {
			h = ph;
		}
	}

	/* initialize a native window object */
	new_window->surface = driver.surface_new(
		window->content_window->surface, x, y, w, h);
	if (new_window->surface == NULL) {
		LOG_PRINT_ERROR("Could not create subsurface!!");
		free(new_window);
		free(window_node);
		return NULL;
	}

	new_window->object = NULL;
	new_window->handler = msghandler;
	new_window->flags = flags;
	new_window->content_window = new_window;
	new_window->node = window_node;
	new_window->paint = paint;
	new_window->user_context = user_context;
	new_window->draw_context = user_context;
	new_window->cairo_context = NULL;
	new_window->parent = window;
	new_window->visible = 1;
	new_window->title = NULL;
	new_window->rect.x = x;
	new_window->rect.y = y;
	new_window->rect.w = w;
	new_window->rect.h = h;
	new_window->foreground_color = window->foreground_color;
	new_window->background_color = window->background_color;
	new_window->decor_dirty = 1;
	new_window->damaged = 0;
	new_window->dirty = 1;
	new_window->stack_node.window = new_window;
	LIST_INIT(&new_window->children);

	/* save a copy of the identifier for debugging */
	if (identifier != NULL) {
		if ((new_window->identifier = strdup(identifier)) == NULL) {
			DEBUG_VPRINT("video", "Could not duplicate window identifier: %s",
				identifier);
		}
	} else {
		new_window->identifier = NULL;
	}

	/* if a message handler was provided create a dispatch
	 * object for it. We also need a message handler if the
	 * parent window has it in order to keep the destructor
	 * semantics the same for windows and subwindows */
	if (msghandler != NULL || window->object != NULL) {
		if ((new_window->object = avbox_object_new(
			avbox_window_handler, new_window)) == NULL) {
			LOG_VPRINT_ERROR("Could not create dispatch object: %s",
				strerror(errno));
			free(window_node);
			free(new_window);
			return NULL;
		}
	}

	/* add the window to the parent window children list */
	window_node->window = new_window;
	LIST_ADD(&window->content_window->children, window_node);
	return new_window;
}


/**
 * Get the window's dirty bit
 */
int
avbox_window_dirty(const struct avbox_window * const window)
{
	return window->dirty;
}


/**
 * Set the window's dirty bit.
 */
void
avbox_window_setdirty(struct avbox_window * const window, int value)
{
	window->dirty = (value != 0);
}


/**
 * Create a new parent window.
 */
struct avbox_window*
avbox_window_new(
	struct avbox_window *parent,
	const char * const identifier,
	int flags,
	const int x, const int y, int w, int h,
	avbox_message_handler msghandler,
	avbox_video_draw_fn draw, void *context)
{
	const char * const title = "NEW WINDOW";
	struct avbox_window *window;
	struct avbox_window_node *window_node;

	/* DEBUG_VPRINT("video", "avbox_window_new(\"%s\")",
		identifier); */

	/* if this is a subwindow invoke a separate
	 * function for now */
	if (flags & AVBOX_WNDFLAGS_SUBWINDOW) {
		assert(parent != NULL);
		return avbox_window_subwindow(parent, identifier, 
			flags, x, y, w, h, msghandler, draw, context);
	}

	assert(parent == NULL);

	/* allocate memory for the window and it's node on the
	 * parent window */
	if ((window = malloc(sizeof(struct avbox_window))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate window object!");
		return NULL;
	}
	if ((window_node = malloc(sizeof(struct avbox_window_node))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate window node. Out of memory");
		free(window);
		return NULL;
	}

	/* initialize a surface for this window */
	window->surface = driver.surface_new(NULL, x, y, w, h);
	if (window->surface == NULL) {
		LOG_PRINT_ERROR("Could not create window surface!");
		free(window_node);
		free(window);
		return NULL;
	}

	window->object = NULL;
	window->handler = msghandler;
	window->flags = flags;
	window->content_window = window;
	window->node = window_node;
	window->title = NULL;
	window->rect.x = x;
	window->rect.y = y;
	window->rect.w = w;
	window->rect.h = h;
	window->foreground_color = MBV_DEFAULT_FOREGROUND;
	window->background_color = MBV_DEFAULT_BACKGROUND;
	window->cairo_context = NULL;
	window->user_context = context;
	window->draw_context = context;
	window->parent = &root_window;
	window->visible = 0;
	window->decor_dirty = 1;
	window->dirty = 1;
	window->damaged = 0;
	window->stack_node.window = window;

	LIST_INIT(&window->children);

	/* save a copy of the identifier if provided */
	if (identifier != NULL) {
		window->identifier = strdup(identifier);
	} else {
		window->identifier = NULL;
	}

	/* if a message handler was provided create a dispatch
	 * object for it */
	if (msghandler != NULL) {
		if ((window->object = avbox_object_new(
			avbox_window_handler, window)) == NULL) {
			LOG_VPRINT_ERROR("Could not create dispatch object: %s",
				strerror(errno));
			free(window_node);
			free(window);
			return NULL;
		}
	}

	/* add the window to the root window's children list */
	window_node->window = window;
	LIST_ADD(&root_window.children, window_node);

	if (flags & AVBOX_WNDFLAGS_DECORATED) {
		char *cidentifier;
		int font_height = default_font_height;
		int subflags = flags;

		subflags &= ~AVBOX_WNDFLAGS_DECORATED;
		subflags &= ~AVBOX_WNDFLAGS_INPUT;
		subflags |= AVBOX_WNDFLAGS_SUBWINDOW;

		/* create a copy of the identifier with _content
		 * appended */
		if ((cidentifier = malloc(strlen(identifier) + 8 + 1)) != NULL) {
			strcpy(cidentifier, identifier);
			strcat(cidentifier, "_content");
		}

		window->paint = &avbox_window_paintdecor;
		window->content_window = avbox_window_new(window,
			cidentifier, subflags, 5, (font_height + (FONT_PADDING)),
			w - 10, h - (font_height + (FONT_PADDING)) - 5,
			NULL,
			draw, NULL);
		if (window->content_window == NULL) {
			driver.surface_destroy(window->surface);
			free(cidentifier);
			free(window);
			return NULL;
		}

		avbox_window_settitle(window, title);
		free(cidentifier);
	} else {
		window->paint = draw;
	}

	/* If a paint handler was not provided clear the window.
	 * This is needed since without a paint handler only areas
	 * covered by widgets will get painted. */
	if (draw == NULL) {
		/* DEBUG_VPRINT("video", "Clearing window %s",
			window->identifier); */
		__window_clear(window, window->background_color);
	}

	return window;
}


/**
 * Sets the drawing function.
 */
void
avbox_window_setdrawfunc(struct avbox_window * const window, avbox_video_draw_fn func, void * const context)
{
	ASSERT(window != NULL);
	ASSERT(window->content_window != NULL);
	window->content_window->paint = func;
	window->draw_context = context;
}


struct avbox_window*
avbox_video_getrootwindow(int screen)
{
	(void) screen;
	return &root_window;
}


/**
 * Causes the window to be repainted.
 */
void
avbox_window_update(struct avbox_window *window)
{
	/* if we're updating the root window then we we don't
	 * need to flip the windows to the front (screen) buffer
	 * as the whole surface will be flipped. This value is
	 * passed down the repaint chain so that none of the
	 * windows get blitted. When we call driver.surface_update()
	 * bellow the whole back-buffer will be flipped */
	const int update = (window != &root_window);

	if (!window->visible) {
		DEBUG_PRINT("video", "Not updating invisible window");
		return;
	}

#ifdef FORCE_FULL_SCREEN_REPAINTS
	if (update) {
		avbox_window_paint(&root_window, 0);
	} else {
		avbox_window_paint(window, 0);
	}
#else
	avbox_window_paint(window, update);
#endif
}


/**
 * Gets the window's canvas size.
 */
void
avbox_window_getcanvassize(const struct avbox_window * const window,
	int * const width, int * const height)
{
	*width = window->content_window->rect.w;
	*height = window->content_window->rect.h;
}


void
avbox_window_setcolor(struct avbox_window *window, uint32_t color)
{
	assert(window != NULL);
	window->foreground_color = color;
}


/**
 * Sets the window's background color.
 */
void
avbox_window_setbgcolor(struct avbox_window * const window,
	const uint32_t color)
{
	assert(window != NULL);
	window->background_color = color;
}

/**
 * Gets the window's foreground color
 */
uint32_t
avbox_window_getcolor(const struct avbox_window *window)
{
	assert(window != NULL);
	return window->foreground_color;
}


/**
 * Gets the window's background color.
 */
uint32_t
avbox_window_getbackground(const struct avbox_window *window)
{
	assert(window != NULL);
	return window->background_color;
}


/**
 * Draws a line on a window.
 */
void
avbox_window_drawline(struct avbox_window *window,
	int x1, int y1, int x2, int y2)
{
	cairo_t *context;

	assert(window != NULL);

	if ((context = avbox_window_cairo_begin(window)) != NULL) {
		cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->foreground_color));
		cairo_set_line_width(context, 2.0);
		cairo_move_to(context, x1, y1);
		cairo_line_to(context, x2, y2);
		cairo_stroke(context);

		avbox_window_cairo_end(window);
	} else {
		fprintf(stderr, "video: Could not get cairo context\n");
	}
}


void
avbox_window_drawstring(struct avbox_window *window,
	char *str, int x, int y)
{
	PangoLayout *layout;
	cairo_t *context;
	int window_width, window_height;


	assert(window != NULL);

	if (str == NULL) {
		DEBUG_PRINT("video", "Did not draw null string");
		return;
	}

	/* TODO: Rewrite this using cairo directly. Because we need
	 * to guarantee that this function succeeds */

	avbox_window_getcanvassize(window, &window_width, &window_height);

	if ((context = avbox_window_cairo_begin(window)) != NULL) {

		cairo_translate(context, 0, 0);

		if ((layout = pango_cairo_create_layout(context)) != NULL) {

			/* DEBUG_VPRINT("video", "Drawing string (x=%i,y=%i,w=%i,h=%i): '%s'",
				x, y, window_width, window_height, str); */

			pango_layout_set_font_description(layout, font_desc);
			pango_layout_set_width(layout, window_width * PANGO_SCALE);
			pango_layout_set_height(layout, window_height * PANGO_SCALE);
			pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
			pango_layout_set_text(layout, str, -1);

			cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->foreground_color));
			pango_cairo_update_layout(context, layout);
			pango_cairo_show_layout(context, layout);

			g_object_unref(layout);

		} else {
			DEBUG_PRINT("video", "Could not create layout");
		}
		avbox_window_cairo_end(window);
	} else {
		DEBUG_PRINT("video", "Could not get cairo context");
	}
}


/**
 * Show the window.
 */
void
avbox_window_show(struct avbox_window * const window)
{
	/* DEBUG_VPRINT("video", "avbox_window_show(0x%p)",
		window); */

	int blitflags = MBV_BLITFLAGS_NONE;
	assert(window != &root_window);

	if (window->visible) {
		DEBUG_VPRINT("video", "WARNING!!: Called avbox_window_show(\"%s\") on visible window",
			window->identifier);
	}

	if (window->flags & AVBOX_WNDFLAGS_ALPHABLEND) {
		blitflags |= MBV_BLITFLAGS_ALPHABLEND;
	}

	/* add to the visible windows stack */
	LIST_APPEND(&window_stack, &window->stack_node);
	window->visible = 1;
#ifdef FORCE_FULL_SCREEN_REPAINTS
	avbox_window_paint(&root_window, 0);
#else
	avbox_window_paint(window, 1);
#endif

	/* if the window has input grab it */
	if (window->flags & AVBOX_WNDFLAGS_INPUT) {
		avbox_input_grab(window->object);
	}
}


/**
 * Hide the window and repair damaged regions
 */
void
avbox_window_hide(struct avbox_window *window)
{
	struct avbox_window_node *damaged_window;

	/* DEBUG_VPRINT("video", "avbox_window_hide(\"%s\")",
		window->identifier); */

	ASSERT(window != &root_window);

	/* if the window is already hidden print a debug message */
	if (!window->visible) {
		DEBUG_PRINT("video", "Hiding invisible window!");
	}

	/* remove window from the stack */
	LIST_REMOVE(&window->stack_node);

	/* if the window has input release it */
	if (window->flags & AVBOX_WNDFLAGS_INPUT) {
		ASSERT(window->object != NULL);
		avbox_input_release(window->object);
	}

	window->visible = 0;

	/* Redraw all damaged windows. For now we just redraw
	 * all the windows that are higher on the stack. */
	LIST_FOREACH(struct avbox_window_node*, damaged_window, &window_stack) {
		if (avbox_rect_overlaps(&window->rect, &damaged_window->window->rect)) {
			/* DEBUG_VPRINT("video", "Repainting damaged window \"%s\"",
				damaged_window->window->identifier); */
			damaged_window->window->damaged = (damaged_window->window == &root_window);
			avbox_window_update(damaged_window->window);
			damaged_window->window->damaged = 0;
			if (avbox_rect_covers(&damaged_window->window->rect, &window->rect)) {
				break;
			}
		}
	}
}


/**
 * Move window to front.
 */
void
avbox_window_tofront(struct avbox_window *window)
{
	assert(window != NULL);
	LIST_REMOVE(&window->stack_node);
	LIST_APPEND(&window_stack, &window->stack_node);
	avbox_window_update(window);
}


/**
 * Destroy a window object
 */
void
avbox_window_destroy(struct avbox_window * const window)
{
	if (window->object != NULL) {
		avbox_object_destroy(window->object);
	} else {
		DEBUG_VPRINT("video", "Destroying window %s right away",
			window->identifier);
		__avbox_window_destroy(window);
		__avbox_window_cleanup(window);
	}
}


#ifdef ENABLE_X11
static void
avbox_video_startx(void)
{
}


static int
avbox_video_can_startx()
{
	return 0;
}
#endif


#ifdef ENABLE_LIBDRM
static int
avbox_video_drm_working()
{
	return 1;
}
#endif


#if defined(ENABLE_DIRECTFB) && \
	(defined(ENABLE_LIBDRM) || defined(ENABLE_X11) || defined(ENABLE_VC4))
static int
avbox_video_directfb_working()
{
	return 1;
}
#endif


/**
 * Initialize the video subsystem.
 */
int
avbox_video_init(int argc, char **argv)
{
	int w = 0, h = 0, i;
#ifdef ENABLE_X11
	int startx = 0;
#endif
	char font_desc_str[16];
	char *driver_string = NULL;

	DEBUG_PRINT("video", "Initializing video subsystem");

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--video:", 8)) {
			char *arg = argv[i] + 8;
			if (!strncmp(arg, "driver=", 7)) {
				driver_string = arg + 7;
#ifdef ENABLE_X11
			} else if (!strcmp("startx", arg)) {
				startx = 1;
#endif
			}
		}
	}

	if (driver_string == NULL) {
#ifdef ENABLE_VC4
		driver_string = "vc4";
#elif defined(ENABLE_LIBDRM)
#ifdef ENABLE_X11
		if (getenv("DISPLAY") != NULL) {
			driver_string = "x11";
		} else if (startx && avbox_video_can_startx()) {
			avbox_video_startx();
			driver_string = "x11";
		} else
#endif
		if (avbox_video_drm_working()) {
			driver_string = "libdrm";
		} else {
#ifdef ENABLE_X11
			if (avbox_video_can_startx()) {
				avbox_video_startx();
				driver_string = "x11";
			} else
#endif
#ifdef ENABLE_DIRECTFB
			if (avbox_video_directfb_working()) {
				driver_string = "directfb";
			} else
#endif
			if (1 /* no driver avail */) {
				driver_string = "null";
			}
		}


/*** We have X11 but no DRM ****/
#elif defined(ENABLE_X11)
		if (getenv("DISPLAY") != NULL) {
			driver_string = "x11";
		} else if (startx && avbox_video_can_startx()) {
			avbox_video_startx();
			driver_string = "x11";
		} else
#ifdef ENABLE_DIRECTFB
		if (avbox_video_directfb_working()) {
			driver_string = "directfb";
		} else
#endif
		if (avbox_video_can_startx()) {
			avbox_video_startx();
			driver_string = "x11";
		} else {
			driver_string = "null";
		}

/*** We only have DirectFB ***/
#elif defined(ENABLE_DIRECTFB)
		driver_string = "directfb";

/*** we have no video :( **/
#else
		driver_string = "";
#endif
	}

	DEBUG_VPRINT("video", "Using '%s' driver",
		driver_string);

	root_window.surface = NULL;

#ifdef ENABLE_LIBDRM
	if (!strcmp(driver_string, "libdrm")) {
		/* attempt to initialize the libdrm driver */
		mbv_drm_initft(&driver);
		root_window.surface = driver.init(&driver, argc, argv, &w, &h);
		if (root_window.surface == NULL) {
			LOG_PRINT_ERROR("Could not initialize libdrm driver!");
		}
	}
#endif

#ifdef ENABLE_VC4
	if (!strcmp(driver_string, "vc4")) {
		/* attempt to initialize the vc4 driver */
		avbox_video_vc4_initft(&driver);
		root_window.surface = driver.init(&driver, argc, argv, &w, &h);
		if (root_window.surface == NULL) {
			LOG_PRINT_ERROR("Could not initialize VC4 driver!");
		}
	}
#endif

#if ENABLE_X11
	if (!strcmp(driver_string, "x11")) {
		avbox_video_x11_initft(&driver);
		root_window.surface = driver.init(&driver, argc, argv, &w, &h);
		if (root_window.surface == NULL) {
			LOG_PRINT_ERROR("Could not initialize X11 driver!");
		}
	}
#endif
#ifdef ENABLE_DIRECTFB
	if (!strcmp(driver_string, "directfb")) {
		/* initialize directfb driver */
		mbv_dfb_initft(&driver);
		root_window.surface = driver.init(&driver, argc, argv, &w, &h);
		if (root_window.surface == NULL) {
			LOG_PRINT_ERROR("Could not initialize DirectFB driver. Exiting!");
		}
	}
#endif

	if (root_window.surface == NULL) {
		LOG_PRINT_ERROR("Could not find a suitable driver!");
		return -1;
	}

	/* initialize the root window */
	root_window.content_window = &root_window;
	root_window.node = NULL;
	root_window.title = NULL;
	root_window.rect.x = 0;
	root_window.rect.y = 0;
	root_window.rect.w = w;
	root_window.rect.h = h;
	root_window.visible = 1;
	root_window.background_color = AVBOX_COLOR(0x000000FF);
	root_window.foreground_color = AVBOX_COLOR(0xFFFFFFFF);
	root_window.user_context = NULL;
	root_window.cairo_context = NULL;
	root_window.parent = NULL;
	root_window.object = NULL;
	root_window.flags = AVBOX_WNDFLAGS_NONE;
	root_window.stack_node.window = &root_window;
	root_window.dirty = 1;

	if ((root_window.identifier = strdup("root_window")) == NULL) {
		ASSERT(errno == ENOMEM);
		driver.shutdown();
		return -1;
	}

	LIST_INIT(&window_stack);
	LIST_INIT(&root_window.children);
	LIST_APPEND(&window_stack, &root_window.stack_node);

	/* calculate default font height based on screen size */
	default_font_height = 16;
	if (w >= 1920) {
		default_font_height = 32;
	} else if (w >= 1280) {
		default_font_height = 28;
	} else if (w >= 1024) {
		default_font_height = 20;
	} else {
		default_font_height = 16;
	}

	/* initialize default font description */
	sprintf(font_desc_str, "Sans Bold %dpx", default_font_height);
	font_desc = pango_font_description_from_string(font_desc_str);
	if (font_desc == NULL) {
		LOG_PRINT_ERROR("Could not initialize font description");
		driver.shutdown();
		free((void*)root_window.identifier);
		return -1;
	}

	return 0;
}


/**
 * Shutdown the graphics subsystem.
 */
void
avbox_video_shutdown()
{
	DEBUG_PRINT("video", "Shutting down graphics system");

	ASSERT(font_desc != NULL);
	ASSERT(root_window.identifier != NULL);

	/* clear screen */
	avbox_window_clear(&root_window);
	avbox_window_update(&root_window);

	/* remove the root window from the
	 * visible window stack */
	LIST_REMOVE(&root_window.stack_node);

	/* free root window resources */
	free((void*)root_window.identifier);

	/* If any windows are still visible then
	 * display a warning.
	 * TODO: keep counters for hidden windows */
#ifndef NDEBUG
	size_t cnt;
	struct avbox_window_node *node;
	LIST_COUNT(&window_stack, cnt);
	if (cnt > 0) {
		DEBUG_VPRINT("video", "LEAK: There are %zd windows on the stack!!",
			cnt);
		LIST_FOREACH(struct avbox_window_node*, node, &window_stack) {
			DEBUG_VPRINT("video", "--> Window: %s",
				node->window->identifier);
		}
	}
#endif

	/* free default font */
	pango_font_description_free(font_desc);

	/* shutdown driver */
	driver.shutdown();
}
