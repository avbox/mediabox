#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pango/pangocairo.h>

#include "video.h"
#include "video-directfb.h"
#include "debug.h"


struct mbv_window
{
	struct mbv_dfb_window *native_window;
	struct mbv_window *content_window;
	const char *title;
	int width;
	int height;
	uint32_t foreground_color;
	uint32_t background_color;
};


static struct mbv_window root_window;
static PangoFontDescription *font_desc;
static int default_font_height = 32;

cairo_t *
mbv_window_cairo_begin(struct mbv_window *window)
{
	return mbv_dfb_window_cairo_begin(window->content_window->native_window);
}


void
mbv_window_cairo_end(struct mbv_window *window)
{
	mbv_dfb_window_cairo_end(window->content_window->native_window);
}


static int
mbv_getfontsize(PangoFontDescription *desc)
{
	int sz;
	sz = pango_font_description_get_size(desc);

	if (!pango_font_description_get_size_is_absolute(desc)) {
		sz = (sz * 96) / (PANGO_SCALE * 72);
	}

	return sz;
}


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
	return mbv_dfb_window_isvisible(window->native_window);
}


int
mbv_window_getsize(struct mbv_window *window, int *width, int *height)
{
	*width = window->width;
	*height = window->height;
	return 0;
}


/**
 * mbv_window_settitle() -- Sets the window title.
 */
int
mbv_window_settitle(struct mbv_window *window, const char *title)
{
	cairo_t *context;
	PangoLayout *layout;
	int font_height = 36;
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

	if ((context = mbv_dfb_window_cairo_begin(window->native_window)) != NULL) {

		/* first clear the title window */
		cairo_move_to(context, 0, 0);
		cairo_line_to(context, window->width, 0);
		cairo_line_to(context, window->width, window->height);
		cairo_line_to(context, 0, window->height);
		cairo_line_to(context, 0, 0);
		cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->background_color));
		cairo_fill(context);

		if ((layout = pango_cairo_create_layout(context)) != NULL) {

			DEBUG_VPRINT("video", "Font size %i",
				mbv_getfontsize(font_desc) / PANGO_SCALE);

			pango_layout_set_font_description(layout, font_desc);
			pango_layout_set_width(layout, window->width * PANGO_SCALE);
			pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
			pango_layout_set_text(layout, window->title, -1);

			cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(window->foreground_color));
			cairo_move_to(context, 0, 0);
			pango_cairo_update_layout(context, layout);
			pango_cairo_show_layout(context, layout);

			/* draw line after title */
			cairo_set_line_width(context, 2.0);
			cairo_move_to(context, 0, font_height + 6);
			cairo_line_to(context, window->width, font_height + 6);
			cairo_stroke(context);
		} else {
			DEBUG_PRINT("video", "Could not create layout");
		}

		mbv_dfb_window_cairo_end(window->native_window);
	}

	return 0;
}


void
mbv_getscreensize(int *width, int *height)
{
	mbv_dfb_getscreensize(width, height);
}


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


int
mbv_isfbdev(void)
{
	return mbv_dfb_isfbdev();
}


int
mbv_window_blit_buffer(
	struct mbv_window *window, void *buf, int width, int height,
	int x, int y)
{
	return mbv_dfb_window_blit_buffer(window->content_window->native_window, buf, width, height, x, y);
}


struct mbv_window*
mbv_window_new(
	char *title,
	int x,
	int y,
	int width,
	int height)
{
	struct mbv_window *window;
	if ((window = malloc(sizeof(struct mbv_window))) == NULL) {
		fprintf(stderr, "video: Could not allocate window object. Out of memory\n");
		return NULL;
	}
	window->native_window = mbv_dfb_window_new(NULL, x, y, width, height);
	if (window->native_window == NULL) {
		fprintf(stderr, "video: Could not create native window. Out of memory\n");
		free(window);
		return NULL;
	}

	window->content_window = window;
	window->title = NULL;
	window->width = width;
	window->height = height;
	window->foreground_color = MBV_DEFAULT_FOREGROUND;
	window->background_color = MBV_DEFAULT_BACKGROUND;

	if (title != NULL) {
		int font_height = 36;

		window->content_window = mbv_window_getchildwindow(
			window, 0, (font_height + 11), width, height - (font_height + 11));
		if (window->content_window == NULL) {
			mbv_dfb_window_destroy(window->native_window);
			free(window);
			return NULL;
		}

		mbv_window_settitle(window, title);
	}
	return window;
}


struct mbv_window*
mbv_window_getchildwindow(struct mbv_window *window,
	int x, int y, int width, int height)
{
	struct mbv_window *new_window;
	if ((new_window = malloc(sizeof(struct mbv_window))) == NULL) {
		fprintf(stderr, "video: Could not allocate window object. Out of memory\n");
		return NULL;
	}

	/* if width or height is -1 adjust it to the
	 * size of the parent window */
	if (width == -1 || height == -1) {
		int w, h;
		mbv_window_getcanvassize(window, &w, &h);
		if (width == -1) {
			width = w;
		}
		if (height == -1) {
			height = h;
		}
	}

	new_window->native_window = mbv_dfb_window_getchildwindow(
		window->content_window->native_window, x, y, width, height);
	if (new_window->native_window == NULL) {
		fprintf(stderr, "video: Could not create native child window.\n");
		free(new_window);
		return NULL;
	}

	new_window->content_window = new_window;
	new_window->title = NULL;
	new_window->width = width;
	new_window->height = height;
	new_window->foreground_color = window->foreground_color;
	new_window->background_color = window->background_color;

	return new_window;
}


struct mbv_window*
mbv_getrootwindow(void)
{
	return &root_window;
}


void
mbv_window_clear(struct mbv_window *window, uint32_t color)
{
	cairo_t *context;

	if (window->title != NULL) {
		mbv_window_settitle(window, window->title);
	}

	if ((context = mbv_window_cairo_begin(window)) != NULL) {
		int w, h;
		mbv_window_getcanvassize(window, &w, &h);
		cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(color));
		cairo_move_to(context, 0, 0);
		cairo_line_to(context, w, 0);
		cairo_line_to(context, w, h);
		cairo_line_to(context, 0, h);
		cairo_line_to(context, 0, 0);
		cairo_fill(context);
		mbv_window_cairo_end(window);
	}
}


void
mbv_window_update(struct mbv_window *window)
{
	mbv_dfb_window_update(window->native_window);
}


void
mbv_window_getcanvassize(struct mbv_window *window,
	int *width, int *height)
{
	*width = window->content_window->width;
	*height = window->content_window->height;
}


void
mbv_window_setcolor(struct mbv_window *window, uint32_t color)
{
	assert(window != NULL);
	window->foreground_color = color;
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


void
mbv_window_show(struct mbv_window *win)
{
	mbv_dfb_window_show(win->native_window);
}


void
mbv_window_hide(struct mbv_window *win)
{
	mbv_dfb_window_hide(win->native_window);
}


void
mbv_window_destroy(struct mbv_window *window)
{
	assert(window != NULL);
	assert(window->native_window != NULL);

	if (window->title != NULL) {
		free((void*)window->title);
	}

	mbv_dfb_window_destroy(window->native_window);
	free(window);
}


void
mbv_init(int argc, char **argv)
{
	int w, h;

	/* initialize default font description */
	font_desc = pango_font_description_from_string("Sans Bold 36px");
	if (font_desc == NULL) {
		fprintf(stderr, "video: Could not initialize font description. Exiting!\n");
		exit(EXIT_FAILURE);
	}

	/* initialize native driver */
	root_window.native_window = mbv_dfb_init(argc, argv);
	if (root_window.native_window == NULL) {
		fprintf(stderr, "video: Could not initialize native driver. Exiting!\n");
		exit(EXIT_FAILURE);
	}

	mbv_getscreensize(&w, &h);

	root_window.content_window = &root_window;
	root_window.title = NULL;
	root_window.width = w;
	root_window.height = h;
	root_window.background_color = 0x000000FF;
	root_window.foreground_color = 0xFFFFFFFF;

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
	mbv_dfb_destroy();
}

