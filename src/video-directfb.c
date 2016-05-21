#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <directfb.h>

#define DEFAULT_FONT "usr/share/fonts/liberation-fonts/LiberationSerif-Regular.ttf"

struct mbv_window
{
	IDirectFBWindow *dfb_window;
	IDirectFBSurface *surface;
	char *title;
	int x;
	int y;
	int width;
	int height;
};

IDirectFB *dfb = NULL; /* global so input-directfb.c can see it */
static IDirectFBSurface *primary = NULL;
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
	DFBCHECK(primary->GetSize(primary, &screen_width, &screen_height));
	DFBCHECK(primary->FillRectangle(primary, 0, 0, screen_width, screen_height));
	DFBCHECK(primary->Flip(primary, NULL, 0));
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

void
mbv_dfb_window_show(struct mbv_window *win)
{
	IDirectFBSurface *surface;
	DFBCHECK(win->dfb_window->GetSurface(win->dfb_window, &surface));
	DFBCHECK(surface->Flip(surface, NULL, DSFLIP_WAITFORSYNC));
}

void
mbv_dfb_window_hide(struct mbv_window *win)
{
	DFBCHECK(win->dfb_window->SetOpacity(win->dfb_window, 0xff));
	IDirectFBSurface *surface;
	DFBCHECK(win->dfb_window->GetSurface(win->dfb_window, &surface));
	DFBCHECK(surface->Flip(surface, NULL, DSFLIP_WAITFORSYNC));
}

void
mbv_dfb_window_destroy(struct mbv_window *win)
{
	if (win != NULL) {
		fprintf(stderr, "Destroying window\n");
		mbv_dfb_window_hide(win);
		/* DFBCHECK(win->surface->Release(win->surface)); */
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
	DFBSurfaceDescription dsc;

	DFBCHECK(DirectFBInit(&argc, &argv));
	DFBCHECK(DirectFBCreate(&dfb));
	//DFBCHECK (
	dfb->SetCooperativeLevel (dfb, DFSCL_FULLSCREEN);
	//);

	dsc.flags = DSDESC_CAPS;
	dsc.caps  = DSCAPS_PRIMARY | DSCAPS_FLIPPING;

	DFBCHECK(dfb->CreateSurface(dfb, &dsc, &primary));

	/* get primary layer */
	DFBCHECK(dfb->GetDisplayLayer(dfb, DLID_PRIMARY, &layer));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	

	/* load default font */
	DFBFontDescription font_dsc = {
		.flags = DFDESC_HEIGHT,
		.height = 18
	};
	DFBCHECK(dfb->CreateFont(dfb,
		"/usr/share/fonts/liberation-fonts/LiberationSerif-Regular.ttf", &font_dsc, &font));


	mbv_dfb_clear();
	//DFBCHECK(layer->SetColor(layer, 0x80, 0x80, 0xff, 0xff));
	//DFBCHECK(layer->DrawLine(layer, 0, 
	//	screen_height / 2, screen_width - 1, screen_height / 2));
	//DFBCHECK(layer->Flip (layer, NULL, 0));
}

void
mbv_dfb_destroy()
{
	layer->Release(layer);
	font->Release(font);
	primary->Release(primary);
	dfb->Release(dfb);
}

