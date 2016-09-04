#include <stdlib.h>
#include <assert.h>
#include <pango/pangocairo.h>

#include "video.h"
#include "video-directfb.h"
#include "debug.h"


struct mbv_window;


static PangoFontDescription *font_desc;
static unsigned int forecolor;


cairo_t *
mbv_window_cairo_begin(struct mbv_window *window)
{
	return mbv_dfb_window_cairo_begin(window);
}


void
mbv_window_cairo_end(struct mbv_window *window)
{
	mbv_dfb_window_cairo_end(window);
}


/**
 * mbv_getdefaultfont() -- Gets the default system font description.
 */
PangoFontDescription *
mbv_getdefaultfont(void)
{
	return font_desc;
}


int
mbv_window_getsize(struct mbv_window *window, int *width, int *height)
{
	return mbv_dfb_window_getsize(window, width, height);
}


/**
 * mbv_window_settitle() -- Sets the window title.
 */
int
mbv_window_settitle(struct mbv_window *window, char *title)
{
	return mbv_dfb_window_settitle(window, title);
}


void
mbv_getscreensize(int *width, int *height)
{
	mbv_dfb_getscreensize(width, height);
}


struct mbv_font *
mbv_font_new(char *file, int height)
{
	return mbv_dfb_font_new(file, height);
}


void
mbv_font_destroy(struct mbv_font *inst)
{
	mbv_dfb_font_destroy(inst);
}


void
mbv_window_fillrectangle(struct mbv_window *window, int x, int y, int w, int h)
{
	mbv_dfb_window_fillrectangle(window, x, y, w, h);
}


int
mbv_getdefaultfontheight(void)
{
	return mbv_dfb_getdefaultfontheight();
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
	return mbv_dfb_window_blit_buffer(window, buf, width, height, x, y);
}


struct mbv_window*
mbv_window_new(
	char *title,
	int x,
	int y,
	int width,
	int height)
{
	return mbv_dfb_window_new(title, x, y, width, height);
}


struct mbv_window*
mbv_window_getchildwindow(struct mbv_window *window,
	int x, int y, int width, int height)
{
	return mbv_dfb_window_getchildwindow(window, x, y, width, height);
}


struct mbv_window*
mbv_getrootwindow(void)
{
	return mbv_dfb_getrootwindow();
}


void
mbv_window_clear(struct mbv_window *win, uint32_t color)
{
	mbv_dfb_window_clear(win, color);
}


void
mbv_window_update(struct mbv_window *window)
{
	mbv_dfb_window_update(window);
}


void
mbv_window_getcanvassize(struct mbv_window *window,
	int *width, int *height)
{
	mbv_dfb_window_getcanvassize(window, width, height);
}


void
mbv_window_setcolor(struct mbv_window *window, uint32_t color)
{
	forecolor = color;
	mbv_dfb_window_setcolor(window, color);
}


void
mbv_window_drawline(struct mbv_window *window,
	int x1, int y1, int x2, int y2)
{
	mbv_dfb_window_drawline(window, x1, y1, x2, y2);
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

			cairo_set_source_rgba(context, CAIRO_COLOR_RGBA(forecolor));
			pango_cairo_update_layout(context, layout);
			pango_cairo_show_layout(context, layout);
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
	mbv_dfb_window_show(win);
}


void
mbv_window_hide(struct mbv_window *win)
{
	mbv_dfb_window_hide(win);
}


void
mbv_window_destroy(struct mbv_window *win)
{
	mbv_dfb_window_destroy(win);
}


void
mbv_init(int argc, char **argv)
{
	font_desc = pango_font_description_from_string("Sans Bold 36px");
	if (font_desc == NULL) {
		fprintf(stderr, "video: Could not initialize font description. Exiting!\n");
		exit(EXIT_FAILURE);
	}

	mbv_dfb_init(argc, argv);
}


void
mbv_destroy()
{
	assert(font_desc != NULL);

	pango_font_description_free(font_desc);
	mbv_dfb_destroy();
}

