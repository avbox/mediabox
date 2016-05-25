#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <directfb.h>

#include <directfb_windows.h>

//#define DEBUG_MEMORY
#define DEFAULT_FONT "/usr/share/fonts/liberation-fonts/LiberationSerif-Regular.ttf"
#define DEFAULT_FONT_HEIGHT (16)
#define DEFAULT_FOREGROUND  (0xFFFFFFFF)


/* window object structure */
struct mbv_window
{
	struct mbv_window *parent;
	IDirectFBWindow *dfb_window;
	IDirectFBSurface *surface;
	IDirectFBSurface *content;
	char *title;
	int x;
	int y;
	int width;
	int height;
	int visible;
	int font_height;
};


IDirectFB *dfb = NULL; /* global so input-directfb.c can see it */
static IDirectFBDisplayLayer *layer = NULL;
static IDirectFBFont *font = NULL;
#ifdef MULTIAPP
static IDirectFBWindows *windows;
#endif
struct mbv_window *root_window = NULL;
static int screen_width = 0;
static int screen_height = 0;


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


int
mbv_dfb_screen_width_get(void)
{
	return screen_width;
}


int
mbv_dfb_screen_height_get(void)
{
	return screen_height;
}


int
mbv_dfb_window_getsize(struct mbv_window *window, int *width, int *height)
{
	if (window == NULL) {
		return -1;
	}
	*width = window->width;
	*height = window->height;
	return 0;
}


int
mbv_dfb_window_blit_buffer(
	struct mbv_window *window,
	void *buf, int width, int height)
{
	DFBSurfaceDescription dsc;
	IDirectFBSurface *surface;

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
	DFBCHECK(window->content->Blit(window->content, surface, NULL, 0, 0));
	DFBCHECK(window->content->Flip(window->content, NULL, DSFLIP_ONSYNC));
	DFBCHECK(surface->Release(surface));

	return 0;
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
	struct mbv_window *win;
	DFBWindowDescription window_desc = {
		.flags = DWDESC_POSX | DWDESC_POSY | DWDESC_WIDTH | DWDESC_HEIGHT |
			 DWDESC_CAPS | DWDESC_SURFACE_CAPS,
		.caps = DWCAPS_ALPHACHANNEL | DWCAPS_DOUBLEBUFFER /*| DWCAPS_NODECORATION */,
		.surface_caps = 0 /*DSCAPS_PRIMARY | DSCAPS_PREMULTIPLIED | DSCAPS_VIDEOONLY */,
		.posx = posx,
		.posy = posy,
		.width = width,
		.height = height
	};

	/* if this is the root window set as primary */
	if (0 && root_window == NULL) {
		window_desc.caps |= DSCAPS_PRIMARY;
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
	win->width = width;
	win->height = height;
	win->font_height = DEFAULT_FONT_HEIGHT;

	if (0 && root_window == NULL) {
		DFBCHECK(layer->GetWindow(layer, 1, &win->dfb_window));
	} else {
		DFBCHECK(layer->CreateWindow(layer, &window_desc, &win->dfb_window));
	}

	/* set opacity to 100% */
	DFBCHECK(win->dfb_window->SetOpacity(win->dfb_window, 0xff));

	/* get the window surface */
	DFBCHECK(win->dfb_window->GetSurface(win->dfb_window, &win->surface));

	/* set basic drawing flags */
	DFBCHECK(win->surface->SetPorterDuff(win->surface, DSPD_SRC_OVER));
	DFBCHECK(win->surface->SetBlittingFlags(win->surface, DSBLIT_BLEND_ALPHACHANNEL));
	DFBCHECK(win->surface->SetDrawingFlags(win->surface, DSDRAW_BLEND));

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

	DFBCHECK(win->content->SetFont(win->content, font));

	return win;
}


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
mbv_dfb_window_clear(struct mbv_window *win,
	unsigned char r,
	unsigned char g,
	unsigned char b,
	unsigned char a)
{
	DFBCHECK(win->content->Clear(win->content, r, g, b, a));
	DFBCHECK(win->content->Flip(win->content, NULL, 0));
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
	DFBCHECK(window->content->DrawLine(window->content,
		x1, y1, x2, y2));
	if (window->visible) {
		DFBCHECK(window->content->Flip(window->content, NULL, 0));
	}
}


void
mbv_dfb_window_drawstring(struct mbv_window *window,
	char *str, int x, int y)
{
	assert(window != NULL);
	assert(str != NULL);

	DFBCHECK(window->content->DrawString(window->content,
		str, -1, x, y, DSTF_TOP | DSTF_CENTER));
	DFBCHECK(window->content->Flip(window->content, NULL, 0));
}


void
mbv_dfb_window_show(struct mbv_window *window)
{
	assert(window != NULL);

	if (window->parent != NULL) {
		mbv_dfb_window_show(window->parent);
	} else if (!window->visible) {
		window->visible = 1;
		DFBCHECK(window->dfb_window->SetOpacity(window->dfb_window, 0xff));
		DFBCHECK(window->surface->Flip(
			window->surface, NULL, DSFLIP_WAITFORSYNC));
	}
}


void
mbv_dfb_window_hide(struct mbv_window *window)
{
	if (window->visible) {
		DFBCHECK(window->dfb_window->SetOpacity(window->dfb_window, 0x00));
		DFBCHECK(window->surface->Flip(
			window->surface, NULL, DSFLIP_WAITFORSYNC));
		window->visible = 0;
	}
}


/**
 * mbv_dfb_window_destroy() -- Destroy a window
 */
void
mbv_dfb_window_destroy(struct mbv_window *window)
{
	assert(window != NULL);

#ifdef DEBUG_MEMORY
	fprintf(stderr, "mbv: Destroying window (0x%lx)\n",
		(unsigned long) window);
#endif

	mbv_dfb_window_hide(window);

	/* release window surfaces */
	DFBCHECK(window->content->Release(window->content));
	DFBCHECK(window->surface->Release(window->surface));

	/* if this is not a subwindow then destroy the directfb
	 * window object as well */
	if (window->parent == NULL) {
		DFBCHECK(window->dfb_window->Release(window->dfb_window));
	}

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


/**
 * mbv_init() -- Initialize video device
 */
void
mbv_dfb_init(int argc, char **argv)
{
	DFBCHECK(DirectFBInit(&argc, &argv));
	DFBCHECK(DirectFBCreate(&dfb));
	//DFBCHECK(dfb->SetCooperativeLevel(dfb, DFSCL_NORMAL));
	//DFBCHECK(dfb->SetVideoMode(dfb, 1024, 768, 24));

	/* IDirectFBScreen does not return the correct size on SDL */
	#if 1
	DFBSurfaceDescription dsc;
	dsc.flags = DSDESC_CAPS;
	dsc.caps  = DSCAPS_PRIMARY | DSCAPS_FLIPPING;
	IDirectFBSurface *primary;
	DFBCHECK(dfb->CreateSurface(dfb, &dsc, &primary));
	DFBCHECK(primary->GetSize(primary, &screen_width, &screen_height));
	DFBCHECK(primary->Release(primary));
	#endif

	/* enumerate display layers */
	DFBCHECK(dfb->EnumDisplayLayers(dfb, enum_display_layers, NULL));

	/* get primary layer */
	DFBCHECK(dfb->GetDisplayLayer(dfb, DLID_PRIMARY, &layer));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	DFBCHECK(layer->SetBackgroundColor(layer, 0x00, 0x00, 0x00, 0xff));
	DFBCHECK(layer->EnableCursor(layer, 0));
	//DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_SHARED));
	DFBCHECK(layer->SwitchContext(layer, DFB_FALSE));
	

	/* load default font */
	DFBFontDescription font_dsc = { .flags = DFDESC_HEIGHT, .height = 16 };
	DFBCHECK(dfb->CreateFont(dfb, DEFAULT_FONT, &font_dsc, &font));

	#if 0
	IDirectFBScreen *screen;
	DFBCHECK(layer->GetScreen(layer, &screen));
	DFBCHECK(screen->GetSize(screen, &screen_width, &screen_height));
	DFBCHECK(screen->Release(screen));
	#endif

#ifdef MULTIAPP
	/* register a window watcher */
	DFBWindowsWatcher watcher;
	memset(&watcher, 0, sizeof(DFBWindowsWatcher));
	watcher.WindowAdd = (void*) mbv_dfb_window_added;
	DFBCHECK(dfb->GetInterface(dfb, "IDirectFBWindows", NULL, NULL, (void**) &windows));
	DFBCHECK(windows->RegisterWatcher(windows, &watcher, NULL));
#endif

	/* create root window */
	root_window = mbv_dfb_window_new(
		NULL,
		0,
		0,
		screen_width,
		screen_height);
	if (root_window == NULL) {
		fprintf(stderr, "Could not create root window\n");
		//return -1;
	}


}

/**
 * mbv_dfb_destroy() -- Destroy the directfb video driver
 */
void
mbv_dfb_destroy()
{
#ifdef MULTIAPP
	DFBCHECK(windows->Release(windows));
#endif
	layer->Release(layer);
	font->Release(font);
	dfb->Release(dfb);
}

