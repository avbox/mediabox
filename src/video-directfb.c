#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <directfb.h>

#define DEFAULT_FONT "/usr/share/fonts/liberation-fonts/LiberationSerif-Regular.ttf"

struct mbv_window
{
	IDirectFBWindow *dfb_window;
	IDirectFBSurface *surface;
	char *title;
	int x;
	int y;
	int width;
	int height;
	int visible;
};

IDirectFB *dfb = NULL; /* global so input-directfb.c can see it */
static IDirectFBDisplayLayer *layer = NULL;
static IDirectFBFont *font = NULL;
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
		.caps = DWCAPS_ALPHACHANNEL | DWCAPS_DOUBLEBUFFER | DWCAPS_NODECORATION,
		.surface_caps = /*DSCAPS_PRIMARY |*/ DSCAPS_PREMULTIPLIED | DSCAPS_VIDEOONLY,
		.posx = posx,
		.posy = posy,
		.width = width,
		.height = height
	};

	/* first allocate the window structure */
	win = malloc(sizeof(struct mbv_window));
	if (win == NULL) {
		fprintf(stderr, "mbv_dfb_window_new() failed -- out of memory\n");
		return NULL;
	}

	DFBCHECK(layer->CreateWindow(layer, &window_desc, &win->dfb_window));

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
	if (title != NULL) {
		DFBCHECK(win->surface->SetColor(win->surface, 0xff, 0xff, 0xff, 0xff));
		DFBCHECK(win->surface->DrawString(win->surface, title, -1, 15, 15, DSTF_LEFT));
	}
	win->visible = 0;

	return win;
}

void
mbv_dfb_window_clear(struct mbv_window *win,
	unsigned char r,
	unsigned char g,
	unsigned char b,
	unsigned char a)
{
	DFBCHECK(win->surface->Clear(win->surface, r, g, b, a));
}

/**
 * mbv_dfb_window_setcolor() -- Set color for future operations
 */
void
mbv_dfb_window_setcolor(struct mbv_window *window, uint32_t color)
{
	fprintf(stderr, "Setting color to 0x%x, 0x%x, 0x%x, 0x%x\n",
		(color >> 24) & 0xff,
		(color >> 16) & 0xff,
		(color >>  8) & 0xff,
		(color      ) & 0xff);
		
	DFBCHECK(window->surface->SetColor(window->surface,
		(color >> 24) & 0xff,
		(color >> 16) & 0xff,
		(color >>  8) & 0xff,
		(color      ) & 0xff));
}


/**
 * mbv_dfb_window_drawline() -- Draw a line on a window
 */
void
mbv_dfb_window_drawline(struct mbv_window *window,
	int x1, int y1, int x2, int y2)
{
	DFBCHECK(window->surface->DrawLine(window->surface,
		x1, y1, x2, y2));
	if (window->visible) {
		DFBCHECK(window->surface->Flip(window->surface, NULL, 0));
	}
}


void
mbv_dfb_window_show(struct mbv_window *window)
{
	if (!window->visible) {
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
		DFBCHECK(window->dfb_window->SetOpacity(window->dfb_window, 0x80));
		DFBCHECK(window->surface->Flip(
			window->surface, NULL, DSFLIP_WAITFORSYNC));
		window->visible = 0;
	}
}

void
mbv_dfb_window_destroy(struct mbv_window *win)
{
	if (win != NULL) {
		fprintf(stderr, "Destroying window\n");
		mbv_dfb_window_hide(win);
		DFBCHECK(win->surface->Release(win->surface));
		DFBCHECK(win->dfb_window->Release(win->dfb_window));
		free(win);
	}
}

/**
 * mbv_init() -- Initialize video device
 */
void
mbv_dfb_init(int argc, char **argv)
{

	DFBCHECK(DirectFBInit(&argc, &argv));
	DFBCHECK(DirectFBCreate(&dfb));
	//DFBCHECK (
	//dfb->SetCooperativeLevel (dfb, DFSCL_FULLSCREEN);
	//);

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

	/* get primary layer */
	DFBCHECK(dfb->GetDisplayLayer(dfb, DLID_PRIMARY, &layer));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	DFBCHECK(layer->SetBackgroundColor(layer, 0x00, 0x00, 0x00, 0xff));
	

	/* load default font */
	DFBFontDescription font_dsc = { .flags = DFDESC_HEIGHT, .height = 18 };
	DFBCHECK(dfb->CreateFont(dfb, DEFAULT_FONT, &font_dsc, &font));

	#if 0
	IDirectFBScreen *screen;
	DFBCHECK(layer->GetScreen(layer, &screen));
	DFBCHECK(screen->GetSize(screen, &screen_width, &screen_height));
	DFBCHECK(screen->Release(screen));
	#endif

	#if 0
	mbv_dfb_clear();
	DFBCHECK(primary->SetColor(primary, 0x80, 0x80, 0xff, 0xff));
	DFBCHECK(primary->DrawLine(primary, 0, 
		screen_height / 2, screen_width - 1, screen_height / 2));
	DFBCHECK(primary->Flip(primary, NULL, 0));
	#endif
}

/**
 * mbv_dfb_destroy() -- Destroy the directfb video driver
 */
void
mbv_dfb_destroy()
{
	layer->Release(layer);
	font->Release(font);
	dfb->Release(dfb);
}

