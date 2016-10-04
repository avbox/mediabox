#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <directfb.h>
#include <directfb_windows.h>
#include <cairo/cairo.h>

/* for direct rendering */
#include <linux/fb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

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
	IDirectFBWindow *dfb_window;
	IDirectFBSurface *surface;
	DFBRectangle rect;
	cairo_t *cairo_context;
	pthread_mutex_t cairo_lock;
	int visible;
	uint8_t opacity;
};




IDirectFB *dfb = NULL; /* global so input-directfb.c can see it */
static IDirectFBDisplayLayer *layer = NULL;
static int screen_width = 0;
static int screen_height = 0;


/* for direct rendering */
static int is_fbdev = 0;
static int fbdev_fd = -1;
static char *fb_mem = NULL;
static size_t screensize;
static struct fb_fix_screeninfo finfo;
static struct fb_var_screeninfo vinfo;
static uint8_t *screen_mask = NULL;
static DFBRectangle* rects[10];
static int n_rects = 0;


static struct mbv_dfb_window *root_window = NULL;
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
 * mbv_dfb_isfbdev() -- Returns one if we're running on a real framebuffer.
 */
int
mbv_dfb_isfbdev(void)
{
	return is_fbdev;
}


/**
 * mb_player_checkfbdev() -- Checks if there's a framebuffer device suitable
 * for direct rendering
 */
static void
mbv_dfb_checkfbdev(void)
{
	int fd;

	DEBUG_PRINT("player", "Initializing /dev/fb0");

	/* try to gain root */
	if (mb_su_gainroot() == -1) {
		fprintf(stderr, "player: Cannot gain root rights!\n");
	}

	if ((fd = open("/dev/fb0", O_RDWR)) != -1) {
		struct fb_fix_screeninfo finfo;
		struct fb_var_screeninfo vinfo;
		void *fb_mem = NULL;
		long screensize;

		/* get screeninfo */
		if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == -1 ||
			ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
		{
			fprintf(stderr, "player: mb_player_checkfbdev(): ioctl() failed\n");
			is_fbdev = 0;
			goto end;
		}

		/* dump some screen info */
		DEBUG_VPRINT("player", "fbdev: bpp=%i", vinfo.bits_per_pixel);
		DEBUG_VPRINT("player", "fbdev: type=%i", finfo.type);
		DEBUG_VPRINT("player", "fbdev: visual=%i", finfo.visual);
		DEBUG_VPRINT("player", "fbdev: FOURCC (grayscale): '%c%c%c%c'",
			((char*)&vinfo.grayscale)[0], ((char*)&vinfo.grayscale)[1],
			((char*)&vinfo.grayscale)[2], ((char*)&vinfo.grayscale)[3]);
		DEBUG_VPRINT("player", "fbdev: xoffset=%i yoffset=%i r=%i g=%i b=%i"
			"player: r=%i g=%i b=%i\n",
			vinfo.xoffset, vinfo.yoffset,
			vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset,
			vinfo.red.length, vinfo.green.length, vinfo.blue.length);

		/* try to mmap video memory */
		screensize = vinfo.yres_virtual * finfo.line_length;
		fb_mem = mmap(0, screensize, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, (off_t) 0);
		if (fb_mem == MAP_FAILED) {
			fprintf(stderr, "player: mmap() failed\n");
			is_fbdev = 0;
			close(fd);
			goto end;
		}

		/* framebuffer device is good */
		is_fbdev = 1;

		/* unmap memory and cleanup */
		munmap(fb_mem, screensize);
		close(fd);

	} else {
		is_fbdev = 0;
	}
end:
	mb_su_droproot();
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


static void
mbv_dfb_addwindowmask(struct mbv_dfb_window *window)
{
	int i;

	if (window == root_window) {
		return;
	}

	for (i = 0; i < 10; i++) {
		if (rects[i] == NULL) {
			rects[i] = &window->rect;
			break;
		}
	}
	ATOMIC_INC(&n_rects);
	mbv_dfb_regeneratemask();
}


static void
mbv_dfb_removewindowmask(struct mbv_dfb_window *window)
{
	int i;

	if (window == root_window) {
		return;
	}

	for (i = 0; i < 10; i++) {
		if (rects[i] == &window->rect) {
			rects[i] = NULL;
			break;
		}
	}

	ATOMIC_DEC(&n_rects);
	mbv_dfb_regeneratemask();
}


int
mbv_dfb_window_isvisible(struct mbv_dfb_window *window)
{
	assert(window != NULL);

	/* walk the window list until we find the parent window.
	 * If any of the parents windows is invisible so is this one */
	while (window->parent != NULL) {
		if (!window->visible) {
			return 0;
		}
		window = window->parent;
	}

	assert(window != NULL);

	return window->visible;
}


int
mbv_dfb_window_blit_buffer(
	struct mbv_dfb_window *window,
	void *buf, int width, int height, const int x, const int y)
{
	if (LIKELY(is_fbdev && window == root_window)) {
		int x_pos, y_pos, pixelsz;
		unsigned int screen = 0;
		void *fb_buf;

		assert(x == 0 && y == 0);

		pixelsz = vinfo.bits_per_pixel / CHAR_BIT;

#ifdef ENABLE_DOUBLE_BUFFERING
		fb_buf = inst->video_buffer;
#else
		fb_buf = fb_mem;
		(void) ioctl(fbdev_fd, FBIO_WAITFORVSYNC, &screen);
#endif

		if (LIKELY(n_rects == 0)) {
			memcpy(fb_buf, buf, screensize);
		} else {
			for (y_pos = 0; y_pos < vinfo.yres; y_pos++) {
				for (x_pos = 0; x_pos < vinfo.xres; x_pos++) {
					if (LIKELY(!screen_mask[(width * y_pos) + x_pos])) {
						long location = (x_pos + vinfo.xoffset) * pixelsz +
							(y_pos + vinfo.yoffset) * finfo.line_length;
						uint32_t *ppix = (uint32_t*) buf;
						*((uint32_t*)(fb_buf + location)) = *(ppix + (((width * y_pos) + x_pos)));
					}
				}
			}
		}

#ifdef ENABLE_DOUBLE_BUFFERING
		(void) ioctl(fd, FBIO_WAITFORVSYNC, &screen);
		memcpy(fb_mem, inst->video_buffer, inst->bufsz * sizeof(uint8_t));
#endif

	} else {
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
		DFBCHECK(window->surface->Blit(window->surface, surface, NULL, x, y));
		surface->Release(surface);
	}

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
	MB_DEBUG_SET_THREAD_NAME("autoflip_root");

	(void) arg;

	while (!root_window_flipper_exit) {
		pthread_mutex_lock(&root_window_lock);
		DFBCHECK(root_window->surface->Flip(root_window->surface, NULL, DSFLIP_NONE));
		pthread_mutex_unlock(&root_window_lock);
		usleep(33333);
	}
	return NULL;
}


#if 0
void
mbv_dfb_window_resize(struct mbv_dfb_window* window, int w, int h)
{
	window->rect.w = w;
	window->rect.h = h;

	mbv_dfb_regeneratemask();

	if (parent == NULL) {

		assert(window->dfb_window != NULL);

		DFBCHECK(window->dfb_window->ResizeSurface(window->dfb_window, w, h));
	}
}
#endif


/**
 * mbv_dfb_window_new() -- Creates a new window
 */
struct mbv_dfb_window*
mbv_dfb_window_new(
	char *title,
	int posx,
	int posy,
	int width,
	int height)
{
	struct mbv_dfb_window *win;
	DFBWindowDescription window_desc = {
		.flags = DWDESC_POSX | DWDESC_POSY | DWDESC_WIDTH | DWDESC_HEIGHT |
			 DWDESC_CAPS | DWDESC_SURFACE_CAPS,
		.caps = /*DWCAPS_ALPHACHANNEL | |*/ /*DWCAPS_DOUBLEBUFFER | */DWCAPS_NODECORATION,
		.surface_caps = DSCAPS_NONE |  DSCAPS_FLIPPING,
		.posx = posx,
		.posy = posy,
		.width = width,
		.height = height
	};

	assert(title == NULL);

	/* if this is the root window set as primary */
	if (root_window == NULL) {
		window_desc.surface_caps |= DSCAPS_PRIMARY;
	}

	/* first allocate the window structure */
	win = malloc(sizeof(struct mbv_dfb_window));
	if (win == NULL) {
		fprintf(stderr, "mbv_dfb_window_new() failed -- out of memory\n");
		return NULL;
	}


	/* initialize window structure */
	win->parent = NULL;
	win->visible = 0;
	win->rect.w = width;
	win->rect.h = height;
	win->rect.x = posx;
	win->rect.y = posy;
	win->opacity = (uint8_t) ((0xFF * DEFAULT_OPACITY) / 100);
	win->cairo_context = NULL;

	if (pthread_mutex_init(&win->cairo_lock, NULL) != 0) {
		fprintf(stderr, "video-dfb: Could not initialize mutex\n");
		free(win);
		return NULL;
	}

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
 * mbv_dfb_window_cairo_end() -- Ends a cairo drawing session and
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
 * mbv_dfb_window_getchildwindow() -- Creates a new child window
 */
struct mbv_dfb_window*
mbv_dfb_window_getchildwindow(struct mbv_dfb_window *window,
	int x, int y, int width, int height)
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
	inst->dfb_window = window->dfb_window;
	inst->visible = 1;
	inst->cairo_context = NULL;
	inst->rect.w = width;
	inst->rect.h = height;
	inst->rect.x = x;
	inst->rect.y = y;

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


void
mbv_dfb_window_update(struct mbv_dfb_window *window)
{
	assert(window != NULL);

	if (mbv_dfb_window_isvisible(window)) {
		DFBCHECK(window->surface->Flip(window->surface, NULL, DSFLIP_BLIT));
	}
}


void
mbv_dfb_window_show(struct mbv_dfb_window *window)
{
	int visible_changed = 0;

	assert(window != NULL);

	if (!window->visible) {
		window->visible = 1;
		DFBCHECK(window->dfb_window->SetOpacity(window->dfb_window, window->opacity));
		visible_changed = 1;
	}

	if (visible_changed) {
		mbv_dfb_addwindowmask(window);
	}

	mbv_dfb_window_update(window);
}


void
mbv_dfb_window_hide(struct mbv_dfb_window *window)
{
	int visible_changed = 0;
	if (window->visible) {
		DFBCHECK(window->dfb_window->SetOpacity(window->dfb_window, 0x00));
		window->visible = 0;
		visible_changed = 1;
	}
	if (visible_changed) {
		mbv_dfb_removewindowmask(window);
	}

	mbv_dfb_window_update(window);

}


/**
 * mbv_dfb_window_destroy() -- Destroy a window
 */
void
mbv_dfb_window_destroy(struct mbv_dfb_window *window)
{
	assert(window != NULL);

	/* hide the window first */
	mbv_dfb_window_hide(window);

	/* release window surfaces */
	window->surface->Release(window->surface);

	/* if this is not a subwindow then destroy the directfb
	 * window object as well */
	if (window->parent == NULL) {
		window->dfb_window->Release(window->dfb_window);
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
 * mbv_init() -- Initialize video device
 */
struct mbv_dfb_window *
mbv_dfb_init(int argc, char **argv)
{
	int i;
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
		NULL, 0, 0, screen_width, screen_height);
	if (root_window == NULL) {
		fprintf(stderr, "Could not create root window\n");
		abort();
	}
	mbv_dfb_window_show(root_window);

	/* print the pixel format of the root window */
	DFBSurfacePixelFormat pix_fmt;
	DFBCHECK(root_window->surface->GetPixelFormat(root_window->surface, &pix_fmt));
	DEBUG_VPRINT("video-dfb", "Root window pixel format: %s", mbv_dfb_pixfmt_tostring(pix_fmt));

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
	} else {
		mbv_dfb_checkfbdev();
	}
	if (is_fbdev) {
		/* initialize framebuffer for direct rendering */
		mb_su_gainroot();
		if ((fbdev_fd = open("/dev/fb0", O_RDWR)) != -1) {
			if (ioctl(fbdev_fd, FBIOGET_VSCREENINFO, &vinfo) == -1 ||
				ioctl(fbdev_fd, FBIOGET_FSCREENINFO, &finfo) == -1) {
				LOG_VPRINT_ERROR("Direct rendering disabled: %s", strerror(errno));
				is_fbdev = 0;
				close(fbdev_fd);
				fbdev_fd = -1;
			} else {
				screensize = vinfo.yres_virtual * finfo.line_length;
				fb_mem = mmap(0, screensize, PROT_READ | PROT_WRITE,
					MAP_SHARED, fbdev_fd, (off_t) 0);
				if (fb_mem == MAP_FAILED) {
					LOG_VPRINT_ERROR("Direct rendering disabled: %s",
						strerror(errno));
					is_fbdev = 0;
					close(fbdev_fd);
					fbdev_fd = -1;
					fb_mem = NULL;
				}
			}
		} else {
			LOG_VPRINT_ERROR("Direct rendering disabled: %s", strerror(errno));
			is_fbdev = 0;
		}
		mb_su_droproot();
	}

	return root_window;
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
	} else {
		assert(fb_mem != NULL);
		assert(fbdev_fd != -1);
		munmap(fb_mem, screensize);
		close(fbdev_fd);
	}

	mbv_dfb_window_destroy(root_window);
	layer->Release(layer);
	dfb->Release(dfb);
	free(screen_mask);
}

