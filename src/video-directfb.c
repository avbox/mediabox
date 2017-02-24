#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <directfb.h>
#include <cairo/cairo.h>

#define LOG_MODULE "video-dfb"

#include "debug.h"
#include "log.h"
#include "compiler.h"
#include "video.h"
#include "su.h"

/* #define DEBUG_MEMORY */
#define DEFAULT_OPACITY     (MBV_DEFAULT_OPACITY)


/* window object structure */
struct mbv_dfb_window
{
	struct mbv_dfb_window *parent;
	IDirectFBSurface *surface;
	DFBRectangle rect;
	cairo_t *cairo_context;
	pthread_mutex_t cairo_lock;
	uint8_t opacity;
	mbv_repaint_handler repaint_handler;
	struct mbv_window *window;
	int is_subwindow;
};


IDirectFB *dfb = NULL; /* global so input-directfb.c can see it */
static IDirectFBDisplayLayer *layer = NULL;
static int screen_width = 0;
static int screen_height = 0;
static struct mbv_dfb_window *root_window = NULL;


#define DFBCHECK(x)                                         \
{                                                            \
	DFBResult err = x;                                         \
         \
	if (err != DFB_OK)                                         \
	{                                                        \
		fprintf( stderr, "%s <%d>:\n\t", __FILE__, __LINE__ ); \
		DirectFBErrorFatal( #x, err );                         \
	}                                                        \
}

#define DFBCOLOR(color) \
	(color >> 24) & 0xFF, \
	(color >> 16) & 0xFF, \
	(color >>  8) & 0xFF, \
	(color      ) & 0xFF


#ifndef NDEBUG
static char *
mbv_dfb_pixfmt_tostring(DFBSurfacePixelFormat fmt)
{
	static char pix_fmt[256];
	switch (fmt) {
	case DSPF_RGB32:  return "RBG32";
	case DSPF_RGB24:  return "RGB24";
	case DSPF_RGB16:  return "RGB16";
	case DSPF_ARGB:   return "ARGB";
	case DSPF_RGB332: return "RGB332";
	case DSPF_YUY2:   return "YUY2";
	case DSPF_UYVY:   return "UYVY";
	case DSPF_YV12:   return "YV12";
	default:
		sprintf(pix_fmt, "PIXFMT: OTHER: %i", fmt);
		return pix_fmt;
	}
}
#endif


/**
 * Gets the screen width and height
 */
void
mbv_dfb_getscreensize(int *w, int *h)
{
	*w = screen_width;
	*h = screen_height;
}


/**
 * Blits an RGB32 C buffer to the window's surface.
 */
int
mbv_dfb_window_blit_buffer(
	struct mbv_dfb_window *window,
	void *buf, int width, int height, const int x, const int y)
{
	DFBSurfaceDescription dsc;
	static IDirectFBSurface *surface = NULL;

	assert(window != NULL);
	assert(window->surface != NULL);

	dsc.width = width;
	dsc.height = height;
	dsc.flags = DSDESC_HEIGHT | DSDESC_WIDTH | DSDESC_PREALLOCATED | DSDESC_PIXELFORMAT;
	dsc.caps = DSCAPS_NONE;
	dsc.pixelformat = DSPF_RGB32;
	dsc.preallocated[0].data = buf;
	dsc.preallocated[0].pitch = width * 4;
	dsc.preallocated[1].data = NULL;
	dsc.preallocated[1].pitch = 0;

	DFBCHECK(dfb->CreateSurface(dfb, &dsc, &surface));
	DFBCHECK(surface->SetBlittingFlags(surface, DSBLIT_NOFX));
	pthread_mutex_lock(&window->cairo_lock);
	DFBCHECK(window->surface->Blit(window->surface, surface, NULL, x, y));
	pthread_mutex_unlock(&window->cairo_lock);

	/* DFBCHECK(window->surface->ReleaseSource(window->surface)); */
	surface->Release(surface);
	return 0;
}


/**
 * Creates a new window.
 */
struct mbv_dfb_window*
mbv_dfb_window_new(
	struct mbv_window *window,
	int posx,
	int posy,
	int width,
	int height,
	void *repaint_handler)
{
	struct mbv_dfb_window *win;

	/* first allocate the window structure */
	win = malloc(sizeof(struct mbv_dfb_window));
	if (win == NULL) {
		fprintf(stderr, "mbv_dfb_window_new() failed -- out of memory\n");
		return NULL;
	}

	/* initialize window structure */
	win->parent = NULL;
	win->window = window;
	win->rect.w = width;
	win->rect.h = height;
	win->rect.x = posx;
	win->rect.y = posy;
	win->is_subwindow = 0;
	win->opacity = (uint8_t) ((0xFF * DEFAULT_OPACITY) / 100);
	win->cairo_context = NULL;
	win->repaint_handler = repaint_handler;

	if (pthread_mutex_init(&win->cairo_lock, NULL) != 0) {
		fprintf(stderr, "video-dfb: Could not initialize mutex\n");
		free(win);
		return NULL;
	}

	if (root_window == NULL) {
		DFBCHECK(layer->GetSurface(layer, &win->surface));
	} else {
		DFBSurfaceDescription dsc;
		dsc.flags = DSDESC_CAPS | DSDESC_WIDTH | DSDESC_HEIGHT;
		dsc.caps = DSCAPS_NONE;
		dsc.width = width;
		dsc.height = height;
		DEBUG_VPRINT("video-dfb:", "GetSubSurface(x=%i,y=%i,w=%i,h=%i)",
			win->rect.x, win->rect.y, win->rect.w, win->rect.h);
		DFBCHECK(dfb->CreateSurface(dfb, &dsc, &win->surface));
	}

	/* set basic drawing flags */
	DFBCHECK(win->surface->SetBlittingFlags(win->surface, DSBLIT_NOFX));

	return win;
}


/**
 * mbv_dfb_window_cairo_begin() -- Gets a cairo context for drawing
 * to the window
 */
cairo_t *
mbv_dfb_window_cairo_begin(struct mbv_dfb_window *window)
{
	cairo_surface_t *surface;
	int pitch;
	void *buf;

	assert(window != NULL);

	pthread_mutex_lock(&window->cairo_lock);

	assert(window->cairo_context == NULL);

	DFBCHECK(window->surface->Lock(window->surface, DSLF_READ | DSLF_WRITE, &buf, &pitch));

	surface = cairo_image_surface_create_for_data(buf,
		CAIRO_FORMAT_ARGB32, window->rect.w, window->rect.h,
		cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, window->rect.w));
	if (surface == NULL) {
		DFBCHECK(window->surface->Unlock(window->surface));
		return NULL;
	}

	window->cairo_context = cairo_create(surface);
	cairo_surface_destroy(surface);
	if (window->cairo_context == NULL) {
		DFBCHECK(window->surface->Unlock(window->surface));
	}
		
	return window->cairo_context;
}


/**
 * Ends a cairo drawing session and
 * unlocks the surface
 */
void
mbv_dfb_window_cairo_end(struct mbv_dfb_window *window)
{
	assert(window != NULL);
	assert(window->cairo_context != NULL);

	cairo_destroy(window->cairo_context);
	window->cairo_context = NULL;

	DFBCHECK(window->surface->Unlock(window->surface));

	pthread_mutex_unlock(&window->cairo_lock);
}


/**
 * Creates a new child window.
 */
struct mbv_dfb_window*
mbv_dfb_window_getchildwindow(struct mbv_dfb_window *window,
	int x, int y, int width, int height, mbv_repaint_handler repaint_handler)
{
	struct mbv_dfb_window *inst;

	assert(window != NULL);
	assert(width != -1);
	assert(height != -1);

	/* allocate memory for window object */
	inst = malloc(sizeof(struct mbv_dfb_window));
	if (inst == NULL) {
		fprintf(stderr, "mbv: Out of memory\n");
		return NULL;
	}

	/* initialize new window object */
	inst->parent = window;
	inst->window = window->window;
	inst->repaint_handler = repaint_handler;
	inst->cairo_context = NULL;
	inst->rect.w = width;
	inst->rect.h = height;
	inst->rect.x = x;
	inst->rect.y = y;
	inst->is_subwindow = 1;

	if (pthread_mutex_init(&inst->cairo_lock, NULL) != 0) {
		fprintf(stderr, "video-dfb: Could not initialize mutex\n");
		free(window);
		return NULL;
	}

	/* create the sub-window surface */
	DFBRectangle rect = { x, y, width, height };
	DFBCHECK(window->surface->GetSubSurface(window->surface, &rect, &inst->surface));

	return inst;
}


/**
 * Flips the window surface into the root window
 */
void
mbv_dfb_window_update(struct mbv_dfb_window * const window,
	int update)
{
	/* DEBUG_VPRINT("video-dfb", "mbv_dfb_window_update(0x%p)",
		window); */

	assert(window != NULL);

	if (window->is_subwindow) {
		return;
	}

	if (window == root_window) {
		/* if this is the root window simply flip the surface */
		pthread_mutex_lock(&window->cairo_lock);
		DFBCHECK(window->surface->Flip(window->surface, NULL, DSFLIP_ONSYNC));
		pthread_mutex_unlock(&window->cairo_lock);
	} else {
		/* if this is a regular window then blit it to the
		 * root window surface */
		DFBRectangle window_rect;
		window_rect.x = 0;
		window_rect.y = 0;
		window_rect.w = window->rect.w;
		window_rect.h = window->rect.h;
		DFBCHECK(root_window->surface->Blit(
			root_window->surface,
			window->surface,
			&window_rect,
			window->rect.x,
			window->rect.y));

		/* now blit the window to the front buffer. It would
		 * be nice if we could blit directly to the front buffer
		 * above but unfortunately this does not appear to be
		 * supported by DirectFB. We could Lock() the surface
		 * and write directly to it but we'll loose acceleration. */
		if (update) {
			DFBRegion region;
			region.x1 = window->rect.x;
			region.y1 = window->rect.y;
			region.x2 = window->rect.x + window->rect.w;
			region.y2 = window->rect.y + window->rect.h;
			pthread_mutex_lock(&root_window->cairo_lock);
			DFBCHECK(root_window->surface->Flip(root_window->surface,
				&region, DSFLIP_BLIT));
			pthread_mutex_unlock(&root_window->cairo_lock);
		}
	}
}


/**
 * Destroy a window
 */
void
mbv_dfb_window_destroy(struct mbv_dfb_window *window)
{
	assert(window != NULL);

	/* release window surfaces */
	window->surface->Release(window->surface);

	/* free window object */
	free(window);
}


/**
 * enum_display_layers() -- Calledback by dfb to enumerate layers.
 */
static DFBEnumerationResult
enum_display_layers(DFBDisplayLayerID id, DFBDisplayLayerDescription desc, void *data)
{
	fprintf(stderr, "mbv: Found display layer %i\n", id);
	return DFENUM_OK;
}


/**
 * Gets a pointer to the root window
 */
struct mbv_dfb_window*
mbv_dfb_getrootwindow(void)
{
	return root_window;
}


static DFBEnumerationResult
mbv_dfb_video_mode_callback(int width, int height, int bpp, void *arg)
{
	(void) arg;
	DEBUG_VPRINT("video-dfb", "Video mode detected %ix%ix%i", width, height, bpp);
	return DFENUM_OK;
}


/**
 * Initialize video device.
 */
struct mbv_dfb_window *
mbv_dfb_init(struct mbv_window *rootwin, int argc, char **argv)
{
	DFBCHECK(DirectFBInit(&argc, &argv));
	DFBCHECK(DirectFBCreate(&dfb));
	DFBCHECK(dfb->SetCooperativeLevel(dfb, DFSCL_NORMAL));
	DFBCHECK(dfb->EnumVideoModes(dfb, mbv_dfb_video_mode_callback, NULL));

	/* IDirectFBScreen does not return the correct size on SDL */
	DFBSurfaceDescription dsc;
	dsc.flags = DSDESC_CAPS;
	dsc.caps  = DSCAPS_PRIMARY;
	IDirectFBSurface *primary;
	DFBCHECK(dfb->CreateSurface(dfb, &dsc, &primary));
	DFBCHECK(primary->GetSize(primary, &screen_width, &screen_height));
	primary->Release(primary);

	/* enumerate display layers */
	DFBCHECK(dfb->EnumDisplayLayers(dfb, enum_display_layers, NULL));

	/* get primary layer */
	DFBCHECK(dfb->GetDisplayLayer(dfb, DLID_PRIMARY, &layer));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	DFBCHECK(layer->SetBackgroundColor(layer, 0x00, 0x00, 0x00, 0xff));
	DFBCHECK(layer->EnableCursor(layer, 0));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	
	/* create root window */
	root_window = mbv_dfb_window_new(
		rootwin, 0, 0, screen_width, screen_height, NULL);
	if (root_window == NULL) {
		fprintf(stderr, "Could not create root window\n");
		abort();
	}
	mbv_dfb_window_update(root_window, 1);

	/* print the pixel format of the root window */
	DFBSurfacePixelFormat pix_fmt;
	DFBCHECK(root_window->surface->GetPixelFormat(root_window->surface, &pix_fmt));
	DEBUG_VPRINT("video-dfb", "Root window pixel format: %s", mbv_dfb_pixfmt_tostring(pix_fmt));

	return root_window;
}


/**
 * Destroy the directfb video driver.
 */
void
mbv_dfb_destroy()
{
	mbv_dfb_window_destroy(root_window);
	layer->Release(layer);
	dfb->Release(dfb);
}

