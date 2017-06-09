/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pango/pangocairo.h>

#define LOG_MODULE "video"

#include "video.h"
#include "video-drv.h"
#include "video-directfb.h"
#include "input.h"
#include "../debug.h"
#include "../linkedlist.h"
#include "../log.h"
#include "../dispatch.h"

#ifdef ENABLE_LIBDRM
#include "video-drm.h"
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
	struct avbox_dispatch_object *object;
	struct avbox_window *content_window;
	struct avbox_window *parent;
	struct avbox_window_node *node;
	struct avbox_window_node stack_node;
	avbox_video_draw_fn paint;
	cairo_t *cairo_context;
	const char *title;
	const char *identifier; /* used for debugging purposes */
	struct avbox_rect rect;
	int visible;
	int flags;
	int decor_dirty;
	uint32_t foreground_color;
	uint32_t background_color;
	void *user_context;
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

	assert(window != NULL);

	if ((buf = driver.surface_lock(window->surface, MBV_LOCKFLAGS_WRITE, &pitch)) == NULL) {
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
	cairo_surface_destroy(surface);
	if (window->cairo_context == NULL) {
		driver.surface_unlock(window->surface);
	}
		
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

	if (window->title != NULL) {
		avbox_window_settitle(window, window->title);
	}

	if ((buf = avbox_window_lock(window, MBV_LOCKFLAGS_WRITE, &pitch)) == NULL) {
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
	__window_clear(window, window->background_color);
}


/**
 * Gets the window's user context
 */
void *
avbox_window_getusercontext(const struct avbox_window * const window)
{
	return window->user_context;
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
struct avbox_dispatch_object*
avbox_window_getobject(struct avbox_window * const window)
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
avbox_window_roundrectangle(struct avbox_window * window, struct avbox_rect *rect, int border_width)
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
	corner_radius = h / 10.0;
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
	struct avbox_window *window, void *buf, int pitch, int width, int height,
	int x, int y)
{
	int ret;
	ret = driver.surface_blitbuf(
		window->content_window->surface,
		buf, pitch, MBV_BLITFLAGS_NONE, width, height, x, y);
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


/**
 * This is the internal repaint handler
 */
static int
avbox_window_paint(struct avbox_window * const window, int update)
{
	int blitflags = MBV_BLITFLAGS_NONE;
	struct avbox_window_node *damaged_window;

	/* DEBUG_VPRINT("video", "avbox_window_paint(\"%s\")",
		window->identifier); */

	if (!window->visible) {
		return 0;
	}

	if (window->flags & AVBOX_WNDFLAGS_ALPHABLEND) {
		blitflags |= MBV_BLITFLAGS_ALPHABLEND;
	}

	/* if the window has no repaint handler then
	 * just invoke the repaint handler for all child
	 * windows */
	if (window->paint == NULL) {
		struct avbox_window_node *child;
		LIST_FOREACH(struct avbox_window_node *, child, &window->children) {
			avbox_window_paint(child->window, update);
		}

		/* blit window */
		driver.surface_update(window->surface, blitflags, update);
	} else {
		/* invoke the user-defined repaint handler */
		window->paint(window);
		driver.surface_update(window->surface, blitflags, update);
	}

	if (window->parent == &root_window) {
		/* redraw all windows damaged by this window, that is
		 * windows higher up on the stack that overlap */
		damaged_window = LIST_NEXT(struct avbox_window_node*,
			&window->stack_node);
		while (!LIST_ISNULL(&window_stack, damaged_window)) {
			if (avbox_rect_overlaps(&window->rect, &damaged_window->window->rect)) {
				avbox_window_update(damaged_window->window);
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
avbox_window_paintdecor(struct avbox_window * const window)
{
	cairo_t *context;
	PangoLayout *layout;
	int font_height = default_font_height;

	assert(window->content_window != window); /* is a window WITH title */

	/* DEBUG_VPRINT("video", "avbox_window_repaint_decoration(\"%s\")",
		window->identifier); */

	if (window->decor_dirty) {
		if ((context = __window_cairobegin(window)) != NULL) {

			/* first clear the title window */
			cairo_move_to(context, 0, 0);
			cairo_line_to(context, window->rect.w, 0);
			cairo_line_to(context, window->rect.w, font_height + FONT_PADDING);
			cairo_line_to(context, 0, font_height + FONT_PADDING);
			cairo_line_to(context, 0, 0);
			cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->background_color));
			cairo_fill(context);

			if ((layout = pango_cairo_create_layout(context)) != NULL) {

				/* DEBUG_VPRINT("video", "Font size %i",
					mbv_getfontsize(font_desc) / PANGO_SCALE); */

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

				/* draw line after title */
				cairo_set_line_width(context, 2.0);
				cairo_move_to(context, 0, font_height + FONT_PADDING);
				cairo_line_to(context, window->rect.w, font_height + FONT_PADDING);
				cairo_stroke(context);

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
	new_window->flags = flags;
	new_window->content_window = new_window;
	new_window->node = window_node;
	new_window->paint = paint;
	new_window->user_context = user_context;
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
	 * object for it */
	if (msghandler != NULL) {
		if ((window->object = avbox_dispatch_createobject(
			msghandler, 0, window)) == NULL) {
			LOG_VPRINT_ERROR("Could not create dispatch object: %s",
				strerror(errno));
			free(window_node);
			free(window);
			return NULL;
		}
	}

	/* add the window to the parent window children list */
	window_node->window = new_window;
	LIST_ADD(&window->content_window->children, window_node);
	return new_window;
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

	DEBUG_ASSERT("video", (msghandler == NULL ||
		(flags & AVBOX_WNDFLAGS_INPUT) == 0),
		"Invalid arguments!");

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
	window->parent = &root_window;
	window->visible = 0;
	window->decor_dirty = 1;
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
		if ((window->object = avbox_dispatch_createobject(
			msghandler, 0, context)) == NULL) {
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
			cidentifier, subflags, 0, (font_height + (FONT_PADDING)),
			w, h - (font_height + (FONT_PADDING)),
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

	avbox_window_paint(window, update);
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
	avbox_window_paint(window, 1);
	driver.surface_update(window->surface, blitflags, 1);

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

	assert(window != &root_window);

	/* if the window is already hidden print a debug message */
	if (!window->visible) {
		DEBUG_PRINT("video", "Hiding invisible window!");
	}

	/* remove window from the stack */
	LIST_REMOVE(&window->stack_node);

	/* if the window has input release it */
	if (window->flags & AVBOX_WNDFLAGS_INPUT) {
		assert(window->object != NULL);
		avbox_input_release(window->object);
	}

	window->visible = 0;

	/* Redraw all damaged windows. For now we just redraw
	 * all the windows that are higher on the stack. */
	LIST_FOREACH(struct avbox_window_node*, damaged_window, &window_stack) {
		if (avbox_rect_overlaps(&window->rect, &damaged_window->window->rect)) {
			DEBUG_VPRINT("video", "Repainting damaged window \"%s\"",
				damaged_window->window->identifier);
			avbox_window_update(damaged_window->window);
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
	/* DEBUG_VPRINT("video", "avbox_window_destroy(\"%s\")",
		window->identifier); */

	assert(window != NULL);
	assert(window->surface != NULL);
	assert(window->content_window != NULL);
	assert(window != &root_window);

	/* if the window is visible hide it before
	 * destroying it */
	if (window->visible && window->parent == &root_window) {
		avbox_window_hide(window);
	}

	/* remove the window from the parent's children list */
	LIST_REMOVE(window->node);
	free(window->node);

	/* destroy the window's dispatch object */
	if (window->object != NULL) {
		avbox_dispatch_destroyobject(window->object);
	}

	if (window->title != NULL) {
		free((void*)window->title);
	}

	if (window->content_window != window) {
		avbox_window_destroy(window->content_window);
	}

	if (window->identifier) {
		free((void*) window->identifier);
	}

	driver.surface_destroy(window->surface);
	free(window);
}


/**
 * Initialize the video subsystem.
 */
int
avbox_video_init(int argc, char **argv)
{
	int w = 0, h = 0, i;
	char font_desc_str[16];
	char *driver_string = "directfb";

	DEBUG_PRINT("video", "Initializing video subsystem");

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--video:", 8)) {
			char *arg = argv[i] + 8;
			if (!strncmp(arg, "driver=", 7)) {
				driver_string = arg + 7;
			}
		}
	}

	DEBUG_VPRINT("video", "Using '%s' driver",
		driver_string);

	root_window.surface = NULL;

#ifdef ENABLE_LIBDRM
	if (!strcmp(driver_string, "libdrm")) {
		/* attempt to initialize the libdrm driver */
		mbv_drm_initft(&driver);
		root_window.surface = driver.init(argc, argv, &w, &h);
		if (root_window.surface == NULL) {
			LOG_PRINT_ERROR("Could not initialize libdrm driver!");
		}
	}
#endif

	if (!strcmp(driver_string, "directfb")) {
		/* initialize directfb driver */
		mbv_dfb_initft(&driver);
		root_window.surface = driver.init(argc, argv, &w, &h);
		if (root_window.surface == NULL) {
			LOG_PRINT_ERROR("Could not initialize DirectFB driver. Exiting!");
		}
	}

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
	switch (w) {
	case 640:  default_font_height = 16; break;
	case 1024: default_font_height = 20; break;
	case 1280: default_font_height = 32; break;
	case 1920: default_font_height = 32; break;
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
