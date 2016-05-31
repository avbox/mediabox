#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <directfb.h>
#include <directfb_windows.h>

/* #define DEBUG_MEMORY */
#define DEFAULT_FONT        ("/usr/share/fonts/dejavu/DejaVuSansCondensed-Bold.ttf")
#define DEFAULT_FONT_HEIGHT (default_font_height)
#define DEFAULT_FOREGROUND  (0xFFFFFFFF)
#define DEFAULT_OPACITY     (100)


/* window object structure */
struct mbv_window
{
	struct mbv_window *parent;
	IDirectFBWindow *dfb_window;
	IDirectFBSurface *surface;
	IDirectFBSurface *content;
	DFBRectangle rect;
	char *title;
	int visible;
	int font_height;
	uint8_t opacity;
};


static DFBRectangle* rects[10];
static uint8_t *screen_mask = NULL;


IDirectFB *dfb = NULL; /* global so input-directfb.c can see it */
static IDirectFBDisplayLayer *layer = NULL;
static IDirectFBFont *font = NULL;
#ifdef ENABLE_MULTIAPP
static IDirectFBWindows *windows;
#endif
static int screen_width = 0;
static int screen_height = 0;
static int is_fbdev = 0;
static int default_font_height;


static struct mbv_window *root_window = NULL;
static int root_window_flipper_exit = 0;
static pthread_t root_window_flipper;
static pthread_mutex_t root_window_lock = PTHREAD_MUTEX_INITIALIZER;



#define DFBCHECK(x...)                                         \
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


/**
 * mbv_dfb_isfbdev() -- Returns one if we're running on a real framebuffer.
 */
int
mbv_dfb_isfbdev(void)
{
	return is_fbdev;
}


/**
 * mb_video_clear()
 */
void
mbv_dfb_clear(void)
{
#if 0
	DFBCHECK(primary->GetSize(primary, &screen_width, &screen_height));
	DFBCHECK(primary->FillRectangle(primary, 0, 0, screen_width, screen_height));
	DFBCHECK(primary->Flip(primary, NULL, 0));
#endif
}


/* DEPRECATED */
int
mbv_dfb_screen_width_get(void)
{
	return screen_width;
}


/* DEPRECATED */
int
mbv_dfb_screen_height_get(void)
{
	return screen_height;
}


int
mbv_dfb_getdefaultfontheight(void)
{
	return default_font_height;
}


/**
 * mbv_dfb_getscreensize() -- Gets the screen width and height
 */
void
mbv_dfb_getscreensize(int *w, int *h)
{
	*w = screen_width;
	*h = screen_height;
}


/**
 * mbv_dfb_getscreenmask() -- Gets a pointer to the screen mask.
 * Please see the description of mbv_dfb_regeneratemask() for more
 * details
 */
void *
mbv_dfb_getscreenmask(void *buf)
{
	return (void *) screen_mask;
}


/**
 * mbv_dfb_regenerate_mask() -- Generates a mask of all visible windows.
 * Since rendering video to a DirectFB window is very inneficient the
 * media player object will render the video directly to the framebuffer
 * whenever possible. This mask contains areas that are being used by other
 * windows so that the player doesn't draw over them. TODO: When supported
 * this should be done in hardware (layers).
 */
static void
mbv_dfb_regeneratemask(void)
{
	int i, x, y;

	memset(screen_mask, 0, screen_width * screen_height);

	for (i = 0; i < 10; i++) {
		if (rects[i] != NULL) {
			for (y = rects[i]->y; y < (rects[i]->y + rects[i]->h); y++) {
				for (x = rects[i]->x; x < (rects[i]->x + rects[i]->w); x++) {
					*(screen_mask + (y * screen_width) + x) = 1;
				}
			}
		}
	}
}


int
mbv_dfb_window_getsize(struct mbv_window *window, int *width, int *height)
{
	if (window == NULL) {
		return -1;
	}
	*width = window->rect.w;
	*height = window->rect.h;
	return 0;
}


int
mbv_dfb_window_blit_buffer(
	struct mbv_window *window,
	void *buf, int width, int height, int x, int y)
{
#if 1	
	DFBSurfaceDescription dsc;
	static IDirectFBSurface *surface = NULL;

	assert(window != NULL);
	assert(window->content != NULL);

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
	DFBCHECK(window->content->Blit(window->content, surface, NULL, x, y));
	//DFBCHECK(window->content->Flip(window->content, NULL, DSFLIP_ONSYNC));
	surface->Release(surface);
#else
	void *dst;
	int pitch;
	DFBCHECK(window->surface->Lock(window->surface, DSLF_READ | DSLF_WRITE, &dst, &pitch));
	memcpy(dst, buf, height * pitch);
	DFBCHECK(window->surface->Unlock(window->surface));;
	//DFBCHECK(window->surface->Flip(window->surface, NULL, DSFLIP_NONE));
#endif

	return 0;
}


/**
 * mbv_dfb_autofliproot() -- Tries to flip the root window at 30 Hz.
 * When we're rendering video to the root window DirectFB may not be able
 * to keep up with this rate, so by running a separate thread for flipping
 * the video stays synchronized but some frames get dropped. Hhopefully this
 * will only be the case when running inside an X server. When running on the
 * framebuffer the video player should draw directly to the framebuffer).
 */
static void *
mbv_dfb_autofliproot(void *arg)
{
	(void) arg;

	while (!root_window_flipper_exit) {
		pthread_mutex_lock(&root_window_lock);
		DFBCHECK(root_window->surface->Flip(root_window->surface, NULL, DSFLIP_NONE));
		pthread_mutex_unlock(&root_window_lock);
		usleep(33333);
	}
	return NULL;
}


/**
 * mbv_dfb_window_new() -- Creates a new window
 */
struct mbv_window*
mbv_dfb_window_new(
	char *title,
	int posx,
	int posy,
	int width,
	int height)
{
	int i;
	struct mbv_window *win;
	DFBWindowDescription window_desc = {
		.flags = DWDESC_POSX | DWDESC_POSY | DWDESC_WIDTH | DWDESC_HEIGHT |
			 DWDESC_CAPS | DWDESC_SURFACE_CAPS,
		.caps = /*DWCAPS_ALPHACHANNEL | |*/ /*DWCAPS_DOUBLEBUFFER | */DWCAPS_NODECORATION,
		.surface_caps = DSCAPS_NONE,
		.posx = posx,
		.posy = posy,
		.width = width,
		.height = height
	};

	/* if this is the root window set as primary */
	if (root_window == NULL) {
		window_desc.surface_caps |= DSCAPS_PRIMARY;
	}

	/* first allocate the window structure */
	win = malloc(sizeof(struct mbv_window));
	if (win == NULL) {
		fprintf(stderr, "mbv_dfb_window_new() failed -- out of memory\n");
		return NULL;
	}

	/* if a window title was specified save a copy */
	if (title != NULL) {
		win->title = strdup(title);
		if (win->title == NULL) {
			fprintf(stderr, "mbv: Out of memory\n");
			free(win);
			return NULL;
		}
	} else {
		win->title = NULL;
	}

	/* initialize window structure */
	win->parent = NULL;
	win->visible = 0;
	win->rect.w = width;
	win->rect.h = height;
	win->rect.x = posx;
	win->rect.y = posy;
	win->font_height = DEFAULT_FONT_HEIGHT;
	win->opacity = (uint8_t) ((0xFF * DEFAULT_OPACITY) / 100);

	if (0 && root_window == NULL) {
		DFBCHECK(layer->GetWindow(layer, 1, &win->dfb_window));
	} else {
		DFBCHECK(layer->CreateWindow(layer, &window_desc, &win->dfb_window));
	}

	/* set opacity to 100% */
	if (root_window == NULL) {
		DFBCHECK(win->dfb_window->SetOpacity(win->dfb_window, 0xff));
	} else {
		DFBCHECK(win->dfb_window->SetOpacity(win->dfb_window, win->opacity));
	}

	/* get the window surface */
	DFBCHECK(win->dfb_window->GetSurface(win->dfb_window, &win->surface));

	/* set basic drawing flags */
	//if (root_window != NULL) {
	//	DFBCHECK(win->surface->SetBlittingFlags(win->surface, DSBLIT_BLEND_ALPHACHANNEL));
	//} else {
		DFBCHECK(win->surface->SetBlittingFlags(win->surface, DSBLIT_NOFX));
	//}
	//DFBCHECK(win->surface->SetPorterDuff(win->surface, DSPD_SRC_OVER));
	//DFBCHECK(win->surface->SetDrawingFlags(win->surface, DSDRAW_BLEND));

	/* clear window */
	DFBCHECK(win->surface->Clear(win->surface, 0x33, 0x49, 0xff, 0xFF));

	/* set default font as window font */
	DFBCHECK(win->surface->SetFont(win->surface, font));

	/* draw the window title */
	if (win->title != NULL) {
		int offset = win->font_height + 10;
		DFBRectangle rect = { 0, offset + 5, width, (height - (offset + 5)) };
		DFBCHECK(win->surface->SetColor(win->surface, DFBCOLOR(DEFAULT_FOREGROUND)));
		DFBCHECK(win->surface->DrawString(win->surface, win->title, -1,
			width / 2, 5, DSTF_TOPCENTER));
		DFBCHECK(win->surface->DrawLine(win->surface, 5, offset,
			width - 10, offset));
		DFBCHECK(win->surface->GetSubSurface(win->surface, &rect, &win->content));
		DFBCHECK(win->content->SetColor(win->content, DFBCOLOR(DEFAULT_FOREGROUND)));
	} else {
		DFBCHECK(win->surface->GetSubSurface(win->surface, NULL, &win->content));
		DFBCHECK(win->content->SetColor(win->content, DFBCOLOR(DEFAULT_FOREGROUND)));
	}

	/* set the window font */
	DFBCHECK(win->content->SetFont(win->content, font));
	
	/* add window to stack and regenerate mask */
	for (i = 0; i < 10; i++) {
		if (rects[i] == NULL) {
			rects[i] = &win->rect;
			break;
		}
	}
	if (root_window != NULL) {
		mbv_dfb_regeneratemask();
	}

	return win;
}


/**
 * mbv_dfb_window_getcanvassize() -- Gets the width and height of
 * a window's drawing area.
 */
void
mbv_dfb_window_getcanvassize(struct mbv_window *window,
	int *width, int *height)
{
	assert(window != NULL);
	assert(width != NULL);
	assert(height != NULL);

	DFBCHECK(window->content->GetSize(window->content, width, height));
}


/**
 * mbv_dfb_window_getchildwindow() -- Creates a new child window
 */
struct mbv_window*
mbv_dfb_window_getchildwindow(struct mbv_window *window,
	int x, int y, int width, int height)
{
	struct mbv_window *inst;

	/* allocate memory for window object */
	inst = malloc(sizeof(struct mbv_window));
	if (inst == NULL) {
		fprintf(stderr, "mbv: Out of memory\n");
		return NULL;
	}

	/* if width or height is -1 adjust it to the
	 * size of the parent window */
	if (width == -1 || height == -1) {
		int w, h;
		mbv_dfb_window_getcanvassize(window, &w, &h);
		if (width == -1) {
			width = w;
		}
		if (height == -1) {
			height = h;
		}
	}

	/* initialize new window object */
	inst->parent = window;
	inst->dfb_window = window->dfb_window;
	inst->title = NULL;
	inst->visible = 0;
	inst->font_height = window->font_height;

	/* create the sub-window surface */
	DFBRectangle rect = { x, y, width, height };
	DFBCHECK(window->content->GetSubSurface(window->content, &rect, &inst->surface));
	DFBCHECK(inst->surface->GetSubSurface(inst->surface, NULL, &inst->content));

	/* set the default surface properties */
	/* TODO: This shoulld be inherited from the parent window */
	DFBCHECK(inst->content->SetFont(inst->content, font));
	DFBCHECK(inst->content->SetColor(inst->content, DFBCOLOR(DEFAULT_FOREGROUND)));

	return inst;
}


void
mbv_dfb_window_clear(struct mbv_window *win, uint32_t color)
{
	DFBCHECK(win->content->Clear(win->content, DFBCOLOR(color)));
}


/**
 * mbv_dfb_window_setcolor() -- Set color for future operations
 */
void
mbv_dfb_window_setcolor(struct mbv_window *window, uint32_t color)
{
	DFBCHECK(window->content->SetColor(window->content, DFBCOLOR(color)));
}


/**
 * mbv_dfb_window_drawline() -- Draw a line on a window
 */
void
mbv_dfb_window_drawline(struct mbv_window *window,
	int x1, int y1, int x2, int y2)
{
	assert(window != NULL);

	DFBCHECK(window->content->DrawLine(window->content,
		x1, y1, x2, y2));
}


void
mbv_dfb_window_drawstring(struct mbv_window *window,
	char *str, int x, int y)
{
	assert(window != NULL);
	assert(str != NULL);

	DFBCHECK(window->content->DrawString(window->content,
		str, -1, x, y, DSTF_TOP | DSTF_CENTER));
}


void
mbv_dfb_window_update(struct mbv_window *window)
{
	/* fprintf(stderr, "mbv: Updating window\n"); */
	DFBCHECK(window->surface->Flip(window->surface, NULL,
		DSFLIP_WAITFORSYNC |  DSFLIP_BLIT));
}


void
mbv_dfb_window_show(struct mbv_window *window)
{
	assert(window != NULL);

	if (!window->visible) {
		window->visible = 1;
		DFBCHECK(window->dfb_window->SetOpacity(window->dfb_window, window->opacity));
	}

	DFBCHECK(window->surface->Flip(window->surface, NULL, DSFLIP_WAITFORSYNC |  DSFLIP_BLIT));
}


void
mbv_dfb_window_hide(struct mbv_window *window)
{
	if (window->visible) {
		DFBCHECK(window->dfb_window->SetOpacity(window->dfb_window, 0x00));
		window->visible = 0;
	}
	DFBCHECK(window->surface->Flip(window->surface, NULL, DSFLIP_WAITFORSYNC |  DSFLIP_BLIT ));
}


/**
 * mbv_dfb_window_destroy() -- Destroy a window
 */
void
mbv_dfb_window_destroy(struct mbv_window *window)
{
	int i;

	assert(window != NULL);

#ifdef DEBUG_MEMORY
	fprintf(stderr, "mbv: Destroying window (0x%lx)\n",
		(unsigned long) window);
#endif

	mbv_dfb_window_hide(window);

	/* release window surfaces */
	window->content->Release(window->content);
	window->surface->Release(window->surface);

	/* if this is not a subwindow then destroy the directfb
	 * window object as well */
	if (window->parent == NULL) {
		window->dfb_window->Release(window->dfb_window);
	}

	for (i = 0; i < 10; i++) {
		if (rects[i] == &window->rect) {
			rects[i] = NULL;
		}
	}

	mbv_dfb_regeneratemask();

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
struct mbv_window*
mbv_dfb_getrootwindow(void)
{
	return root_window;
}


#ifdef MULTIAPP
static void
mbv_dfb_window_added(void *context, const DFBWindowInfo info)
{
	fprintf(stderr, "Window added: winid=%i resid=%lu procid=%i inst=%i\n",
		info.window_id, info.resource_id, info.process_id, info.instance_id);
}
#endif


static DFBEnumerationResult
mbv_dfb_video_mode_callback(int width, int height, int bpp, void *arg)
{
	(void) arg;
	fprintf(stderr, "mbv: Video mode detected %ix%ix%i\n",
		width, height, bpp);
	return DFENUM_OK;
}


/**
 * mbv_init() -- Initialize video device
 */
void
mbv_dfb_init(int argc, char **argv)
{
	int i;
	DFBCHECK(DirectFBInit(&argc, &argv));
	DFBCHECK(DirectFBCreate(&dfb));
	DFBCHECK(dfb->SetCooperativeLevel(dfb, DFSCL_NORMAL));
	DFBCHECK(dfb->EnumVideoModes(dfb, mbv_dfb_video_mode_callback, NULL));

	/* IDirectFBScreen does not return the correct size on SDL */
	#if 1
	DFBSurfaceDescription dsc;
	dsc.flags = DSDESC_CAPS;
	dsc.caps  = DSCAPS_PRIMARY /*| DSCAPS_FLIPPING*/;
	IDirectFBSurface *primary;
	DFBCHECK(dfb->CreateSurface(dfb, &dsc, &primary));
	DFBCHECK(primary->GetSize(primary, &screen_width, &screen_height));
	primary->Release(primary);
	#endif

	/* enumerate display layers */
	DFBCHECK(dfb->EnumDisplayLayers(dfb, enum_display_layers, NULL));

	/* get primary layer */
	DFBCHECK(dfb->GetDisplayLayer(dfb, DLID_PRIMARY, &layer));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	DFBCHECK(layer->SetBackgroundColor(layer, 0x00, 0x00, 0x00, 0xff));
	DFBCHECK(layer->EnableCursor(layer, 0));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	
	/* load default font */
	default_font_height = 16;
	switch (screen_width) {
	case 640:  default_font_height = 16; break;
	case 1024: default_font_height = 20; break;
	case 1280: default_font_height = 22; break;
	case 1920: default_font_height = 24; break;
	}
	DFBFontDescription font_dsc = { .flags = DFDESC_HEIGHT, .height = default_font_height };
	DFBCHECK(dfb->CreateFont(dfb, DEFAULT_FONT, &font_dsc, &font));

	#if 0
	IDirectFBScreen *screen;
	DFBCHECK(layer->GetScreen(layer, &screen));
	DFBCHECK(screen->GetSize(screen, &screen_width, &screen_height));
	DFBCHECK(screen->Release(screen));
	#endif

#ifdef ENABLE_MULTIAPP
	/* register a window watcher */
	DFBWindowsWatcher watcher;
	memset(&watcher, 0, sizeof(DFBWindowsWatcher));
	watcher.WindowAdd = (void*) mbv_dfb_window_added;
	DFBCHECK(dfb->GetInterface(dfb, "IDirectFBWindows", NULL, NULL, (void**) &windows));
	DFBCHECK(windows->RegisterWatcher(windows, &watcher, NULL));
#endif

	/* create root window */
	root_window = mbv_dfb_window_new(
		NULL, 0, 0, screen_width, screen_height);
	if (root_window == NULL) {
		fprintf(stderr, "Could not create root window\n");
		abort();
	}

	/* print the pixel format of the root window */
	DFBSurfacePixelFormat pix_fmt;
	DFBCHECK(root_window->content->GetPixelFormat(root_window->content, &pix_fmt));
	fprintf(stderr, "mbv: Root window pixel format: %s\n", mbv_dfb_pixfmt_tostring(pix_fmt));

	/* for now one byte per pixel */
	screen_mask = malloc(screen_width * screen_height);
	if (screen_mask == NULL) {
		fprintf(stderr, "mbv: malloc() failed\n");
		abort();
	}

	/* regenerate the screen mask */
	mbv_dfb_regeneratemask();
	for (i = 0; i < 10; i++) {
		rects[i] = NULL;
	}

	/* try to detect if we're running inside an X server */
	/* TODO: Use a more reliable way */
	is_fbdev = (getenv("DISPLAY") == NULL);
	if (!is_fbdev) {
		if (pthread_create(&root_window_flipper, NULL, mbv_dfb_autofliproot, NULL) != 0) {
			fprintf(stderr, "mbv: Could not create autoflip thread\n");
			abort();
		}
	}
}

/**
 * mbv_dfb_destroy() -- Destroy the directfb video driver
 */
void
mbv_dfb_destroy()
{
	if (!is_fbdev) {
		root_window_flipper_exit = 1;
		pthread_join(root_window_flipper, NULL);
	}

	mbv_dfb_window_destroy(root_window);
#ifdef ENABLE_MULTIAPP
	DFBCHECK(windows->Release(windows));
#endif
	layer->Release(layer);
	font->Release(font);
	dfb->Release(dfb);
	free(screen_mask);
}

