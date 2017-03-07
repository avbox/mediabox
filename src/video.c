/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pango/pangocairo.h>

#define LOG_MODULE "video"

#include "video.h"
#include "video-drv.h"
#include "video-directfb.h"
#include "debug.h"
#include "linkedlist.h"
#include "log.h"


/**
 * Represents a rectangle.
 */
struct mbv_rect
{
	int x;
	int y;
	int w;
	int h;
};


/**
 * Represents a window object.
 */
struct mbv_window
{
	struct mbv_surface *surface;
	struct mbv_window *content_window;
	struct mbv_window *parent;
	mbv_paint_func paint;
	cairo_t *cairo_context;
	const char *title;
	const char *identifier; /* used for debugging purposes */
	struct mbv_rect rect;
	int visible;
	int decor_dirty;
	uint32_t foreground_color;
	uint32_t background_color;
	void *user_context;
	LIST_DECLARE(children);
};


LISTABLE_STRUCT(mbv_childwindow,
	struct mbv_window *window;
);

static struct mbv_drv_funcs driver;
static struct mbv_window root_window;
static PangoFontDescription *font_desc;
static int default_font_height = 32;


/**
 * Checks if inner is inside outter. Returns 1 if
 * it is, 0 otherwise
 */
static int
mbv_rect_isinside(const struct mbv_rect * const inner,
	const struct mbv_rect * const outter)
{
	return inner->x >= outter->x && inner->y >= outter->y &&
		(inner->x + inner->w) <= (outter->x + outter->w) &&
		(inner->y + inner->h) <= (outter->y + outter->h);
}



/**
 * Gets the cairo context for a window. This is the
 * internal version that works directly on the surface
 * (and not the content window subsurface).
 */
static cairo_t *
__window_cairobegin(struct mbv_window * const window)
{
	cairo_surface_t *surface;
	int pitch;
	void *buf;

	assert(window != NULL);

	if ((buf = driver.surface_lock(window->surface, &pitch)) == NULL) {
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
__window_cairoend(struct mbv_window *window)
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
__window_clear(struct mbv_window *window, const uint32_t color)
{
	cairo_t *context;

	if (window->title != NULL) {
		mbv_window_settitle(window, window->title);
	}

	if ((context = __window_cairobegin(window)) != NULL) {
		int w, h;
		mbv_window_getsize(window, &w, &h);
		cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(color));
		cairo_move_to(context, 0, 0);
		cairo_line_to(context, w, 0);
		cairo_line_to(context, w, h);
		cairo_line_to(context, 0, h);
		cairo_line_to(context, 0, 0);
		cairo_fill(context);
		__window_cairoend(window);
	}
}


/**
 * Gets the cairo context for a window
 */
cairo_t *
mbv_window_cairo_begin(struct mbv_window * const window)
{
	return __window_cairobegin(window->content_window);
}


/**
 * Releases the cairo context for the window
 */
void mbv_window_cairo_end(struct mbv_window * const window)
{
	assert(window != NULL);
	__window_cairoend(window->content_window);
}

/**
 * Clear the window.
 */
void mbv_window_clear(struct mbv_window * const window)
{
	assert(window != NULL);
	__window_clear(window, window->background_color);
}


/**
 * Gets the window's user context
 */
void *
mbv_window_getusercontext(const struct mbv_window * const window)
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
 * mbv_window_isvisible() -- Checks if the given window is visible.
 */
int
mbv_window_isvisible(struct mbv_window *window)
{
	return window->visible;
}


int
mbv_window_getsize(struct mbv_window *window, int *width, int *height)
{
	*width = window->rect.w;
	*height = window->rect.h;
	return 0;
}


/**
 * mbv_window_settitle() -- Sets the window title.
 */
int
mbv_window_settitle(struct mbv_window *window, const char *title)
{
	char *title_copy;

	assert(window->content_window != window); /* is a window WITH title */

	title_copy = strdup(title);
	if (title_copy == NULL) {
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
mbv_window_fillrectangle(struct mbv_window *window, int x, int y, int w, int h)
{
	cairo_t *context;

	if ((context = mbv_window_cairo_begin(window)) != NULL) {
		cairo_move_to(context, x, y);
		cairo_line_to(context, x + w, y);
		cairo_line_to(context, x + w, y + h);
		cairo_line_to(context, x, y + h);
		cairo_line_to(context, x, y);
		cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->foreground_color));
		cairo_fill(context);
		mbv_window_cairo_end(window);
	}
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
mbv_window_blitbuf(
	struct mbv_window *window, void *buf, int width, int height,
	int x, int y)
{
	int ret;
	ret = driver.surface_blitbuf(
		window->content_window->surface,
		buf, width, height, x, y);
	return ret;
}


/**
 * This is the internal repaint handler
 */
static int
mbv_window_paint(struct mbv_window * const window, int update)
{
	/* DEBUG_VPRINT("video", "mbv_window_paint(\"%s\")",
		window->identifier); */

	if (!window->visible) {
		return 0;
	}

	/* if the window has no repaint handler then
	 * just invoke the repaint handler for all child
	 * windows */
	if (window->paint == NULL) {
		struct mbv_childwindow *child;
		LIST_FOREACH(struct mbv_childwindow *, child, &window->children) {
			mbv_window_paint(child->window, update);
		}

		/* blit window */
		driver.surface_update(window->surface, update);
		return 0;
	} else {
		/* invoke the user-defined repaint handler */
		window->paint(window);
		driver.surface_update(window->surface, update);
		return 0;
	}
}


/**
 * Repaints the window decoration
 */
static int
mbv_window_paintdecor(struct mbv_window * const window)
{
	cairo_t *context;
	PangoLayout *layout;
	int font_height = 36;

	assert(window->content_window != window); /* is a window WITH title */

	/* DEBUG_VPRINT("video", "mbv_window_repaint_decoration(\"%s\")",
		window->identifier); */

	if (window->decor_dirty) {
		if ((context = __window_cairobegin(window)) != NULL) {

			/* first clear the title window */
			cairo_move_to(context, 0, 0);
			cairo_line_to(context, window->rect.w, 0);
			cairo_line_to(context, window->rect.w, font_height + 6);
			cairo_line_to(context, 0, font_height + 6);
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
				cairo_move_to(context, 0, font_height + 6);
				cairo_line_to(context, window->rect.w, font_height + 6);
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
	return mbv_window_paint(window->content_window, 1);
}


/**
 * Create a new parent window.
 */
struct mbv_window*
mbv_window_new(
	const char * const identifier,
	char *title, const int x, const int y, int w, int h,
	mbv_paint_func paint)
{
	struct mbv_window *window;
	struct mbv_childwindow *window_node;

	DEBUG_VPRINT("video", "mbv_window_new(\"%s\")",
		identifier);

	/* allocate memory for the window and it's node on the
	 * parent window */
	if ((window = malloc(sizeof(struct mbv_window))) == NULL) {
		fprintf(stderr, "video: Could not allocate window object. Out of memory\n");
		return NULL;
	}
	if ((window_node = malloc(sizeof(struct mbv_childwindow))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate window node. Out of memory");
		free(window);
		return NULL;
	}

	/* initialize a surface for this window */
	window->surface = driver.surface_new(NULL, x, y, w, h);
	if (window->surface == NULL) {
		fprintf(stderr, "video: Could not create native window. Out of memory\n");
		free(window);
		return NULL;
	}

	window->content_window = window;
	window->title = NULL;
	window->rect.x = x;
	window->rect.y = y;
	window->rect.w = w;
	window->rect.h = h;
	window->foreground_color = MBV_DEFAULT_FOREGROUND;
	window->background_color = MBV_DEFAULT_BACKGROUND;
	window->cairo_context = NULL;
	window->user_context = NULL;
	window->parent = &root_window;
	window->visible = 0;
	window->decor_dirty = 1;

	LIST_INIT(&window->children);

	/* save a copy of the identifier if provided */
	if (identifier != NULL) {
		window->identifier = strdup(identifier);
	} else {
		window->identifier = NULL;
	}

	/* add the window to the root window's children list */
	window_node->window = window;
	LIST_ADD(&root_window.children, window_node);

	if (title != NULL) {
		char *cidentifier;
		int font_height = 36;

		/* create a copy of the identifier with _content
		 * appended */
		if ((cidentifier = malloc(strlen(identifier) + 8 + 1)) != NULL) {
			strcpy(cidentifier, identifier);
			strcat(cidentifier, "_content");
		}

		window->paint = &mbv_window_paintdecor;
		window->content_window = mbv_window_getchildwindow(window,
			cidentifier, 0, (font_height + 11), w, h - (font_height + 11),
			paint, NULL);
		if (window->content_window == NULL) {
			driver.surface_destroy(window->surface);
			free(window);
			return NULL;
		}

		mbv_window_settitle(window, title);
	} else {
		window->paint = paint;
	}

	/* If a paint handler was not provided clear the window.
	 * This is needed since without a paint handler only areas
	 * covered by widgets will get painted. */
	if (paint == NULL) {
		DEBUG_VPRINT("video", "Clearing window %s",
			window->identifier);
		__window_clear(window, window->background_color);
	}

	return window;
}


/**
 * Gets a child window
 */
struct mbv_window*
mbv_window_getchildwindow(struct mbv_window * const window,
	const char * const identifier,
	const int x, const int y, int w, int h, mbv_paint_func paint,
	void *user_context)
{
	struct mbv_window *new_window;
	struct mbv_childwindow *window_node;

	DEBUG_VPRINT("video", "mbv_window_getchildwindow(\"%s\")",
		identifier);

	/* allocate memory for the window and it's node */
	if ((new_window = malloc(sizeof(struct mbv_window))) == NULL) {
		fprintf(stderr, "video: Could not allocate window object. Out of memory\n");
		return NULL;
	}
	if ((window_node = malloc(sizeof(struct mbv_childwindow))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate window node. Out of memory");
		free(new_window);
		return NULL;
	}

	/* if width or height is -1 adjust it to the
	 * size of the parent window */
	if (w == -1 || h == -1) {
		int pw, ph;
		mbv_window_getcanvassize(window, &pw, &ph);
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
		return NULL;
	}

	new_window->content_window = new_window;
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

	/* add the window to the parent window children list */
	window_node->window = new_window;
	LIST_ADD(&window->content_window->children, window_node);
	return new_window;
}


struct mbv_window*
mbv_getrootwindow(void)
{
	return &root_window;
}


/**
 * Causes the window to be repainted.
 */
void
mbv_window_update(struct mbv_window *window)
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

	mbv_window_paint(window, update);

	driver.surface_update(window->surface, update);
}


/**
 * Gets the window's canvas size.
 */
void
mbv_window_getcanvassize(const struct mbv_window * const window,
	int * const width, int * const height)
{
	*width = window->content_window->rect.w;
	*height = window->content_window->rect.h;
}


void
mbv_window_setcolor(struct mbv_window *window, uint32_t color)
{
	assert(window != NULL);
	window->foreground_color = color;
}


/**
 * Sets the window's background color.
 */
void
mbv_window_setbgcolor(struct mbv_window * const window,
	const uint32_t color)
{
	assert(window != NULL);
	window->background_color = color;
}

/**
 * Gets the window's foreground color
 */
uint32_t
mbv_window_getcolor(const struct mbv_window *window)
{
	assert(window != NULL);
	return window->foreground_color;
}


/**
 * Gets the window's background color.
 */
uint32_t
mbv_window_getbackground(const struct mbv_window *window)
{
	assert(window != NULL);
	return window->background_color;
}


/**
 * Draws a line on a window.
 */
void
mbv_window_drawline(struct mbv_window *window,
	int x1, int y1, int x2, int y2)
{
	cairo_t *context;

	assert(window != NULL);

	if ((context = mbv_window_cairo_begin(window)) != NULL) {
		cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->foreground_color));
		cairo_set_line_width(context, 2.0);
		cairo_move_to(context, x1, y1);
		cairo_line_to(context, x2, y2);
		cairo_stroke(context);

		mbv_window_cairo_end(window);
	} else {
		fprintf(stderr, "video: Could not get cairo context\n");
	}
}


void
mbv_window_drawstring(struct mbv_window *window,
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

	mbv_window_getcanvassize(window, &window_width, &window_height);

	if ((context = mbv_window_cairo_begin(window)) != NULL) {

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
		mbv_window_cairo_end(window);
	} else {
		DEBUG_PRINT("video", "Could not get cairo context");
	}
}


/**
 * Show the window.
 */
void
mbv_window_show(struct mbv_window * const window)
{
	DEBUG_VPRINT("video", "mbv_window_show(0x%p)",
		window);

	assert(window != &root_window);

	if (window->visible) {
		DEBUG_VPRINT("video", "WARNING!!: Called mbv_window_show(\"%s\") on visible window",
			window->identifier);
	}

	window->visible = 1;
	mbv_window_paint(window, 1);
	driver.surface_update(window->surface, 1);
}


/**
 * Find a single window that can be repainted to repair
 * all damage caused by the window specified by the window
 * argument. The current argument must be set to a window
 * that is to be used as the root of the search. This is
 * usually the root_window. As long as that window can cover
 * all the damage (which root always does) this function
 * is guaranteed to return a pointer to a valid window in the
 * damaged_window argument.
 *
 */
static void
mbv_window_finddamagedwindow(
	const struct mbv_window * const window,
	struct mbv_window * const current,
	struct mbv_window **damaged_window)
{
	if (current->visible &&
		mbv_rect_isinside(&window->rect, &current->rect)) {
		struct mbv_childwindow *child;
		*damaged_window = current;
		LIST_FOREACH(struct mbv_childwindow*, child, &current->children) {
			mbv_window_finddamagedwindow(window, child->window, damaged_window);
		}
	}
}


/**
 * Hide the window and repair damaged regions
 */
void
mbv_window_hide(struct mbv_window *window)
{
	struct mbv_window *damaged_window = NULL;

	DEBUG_VPRINT("video", "mbv_window_hide(\"%s\")",
		window->identifier);

	assert(window != &root_window);

	window->visible = 0;

	/* find and repair the damaged window */
	mbv_window_finddamagedwindow(window, &root_window, &damaged_window);
	assert(damaged_window != NULL);

	DEBUG_VPRINT("video", "Repainting damaged window \"%s\"",
		damaged_window->identifier);

	/* repaint the damaged window */
	mbv_window_update(damaged_window);
}


/**
 * Destroy a window object
 */
void
mbv_window_destroy(struct mbv_window * const window)
{
	/* DEBUG_VPRINT("video", "mbv_window_destroy(\"%s\")",
		window->identifier); */

	assert(window != NULL);
	assert(window->surface != NULL);
	assert(window->content_window != NULL);
	assert(window != &root_window);

	/* if the window is visible hide it before
	 * destroying it */
	if (window->visible && window->parent == &root_window) {
		mbv_window_hide(window);
	}

	/* remove the window from the parent's children list */
	if (window->parent != NULL) {
		struct mbv_childwindow *child;
		LIST_FOREACH(struct mbv_childwindow *, child, &window->parent->children) {
			if (child->window == window) {
				LIST_REMOVE(child);
				free(child);
				break;
			}
		}
	}

	if (window->title != NULL) {
		free((void*)window->title);
	}

	if (window->content_window != window) {
		mbv_window_destroy(window->content_window);
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
void
mbv_init(int argc, char **argv)
{
	int w, h;

	mbv_dfb_initft(&driver);

	/* initialize default font description */
	font_desc = pango_font_description_from_string("Sans Bold 36px");
	if (font_desc == NULL) {
		fprintf(stderr, "video: Could not initialize font description. Exiting!\n");
		exit(EXIT_FAILURE);
	}

	/* initialize native driver */
	root_window.surface = driver.init(argc, argv, &w, &h);
	if (root_window.surface == NULL) {
		fprintf(stderr, "video: Could not initialize native driver. Exiting!\n");
		exit(EXIT_FAILURE);
	}

	root_window.content_window = &root_window;
	root_window.title = NULL;
	root_window.rect.x = 0;
	root_window.rect.y = 0;
	root_window.rect.w = w;
	root_window.rect.h = h;
	root_window.visible = 1;
	root_window.identifier = strdup("root_window");
	root_window.background_color = 0x000000FF;
	root_window.foreground_color = 0xFFFFFFFF;
	root_window.user_context = NULL;
	root_window.cairo_context = NULL;
	root_window.parent = NULL;

	LIST_INIT(&root_window.children);

	/* calculate default font height based on screen size */
	default_font_height = 16;
	switch (w) {
	case 640:  default_font_height = 16; break;
	case 1024: default_font_height = 20; break;
	case 1280: default_font_height = 32; break;
	case 1920: default_font_height = 32; break;
	}

}


void
mbv_destroy()
{
	assert(font_desc != NULL);

	pango_font_description_free(font_desc);
	driver.shutdown();
}

