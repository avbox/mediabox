/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <directfb.h>

#define LOG_MODULE "video-dfb"

#include "../debug.h"
#include "../log.h"
#include "../compiler.h"
#include "../su.h"
#include "video.h"
#include "video-drv.h"

/* #define DEBUG_MEMORY */
#define DEFAULT_OPACITY     (MBV_DEFAULT_OPACITY)


/* window object structure */
struct mbv_surface
{
	IDirectFBSurface *surface;
	DFBRectangle rect;
	pthread_mutex_t lock;
	int is_subwindow;
	void *buf;
};


IDirectFB *dfb = NULL; /* global so input-directfb.c can see it */
static IDirectFBDisplayLayer *layer = NULL;
static struct mbv_surface *root = NULL;

#define ALIGNED(addr, bytes) \
    (((uintptr_t)(const void *)(addr)) % (bytes) == 0)

#define DFBCHECK(x)                                         \
{                                                            \
	DFBResult err = x;                                         \
         \
	if (err != DFB_OK)                                         \
	{                                                        \
		LOG_VPRINT_ERROR("%s <%d>:\n\t", __FILE__, __LINE__ ); \
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
pixfmt_tostring(DFBSurfacePixelFormat fmt)
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
 * Lock a surface and return a pointer for writing
 * to it. The pitch argument will indicate the pitch
 * of the returned surface buffer.
 */
static void *
surface_lock(struct mbv_surface * const inst,
	const unsigned int flags, int *pitch)
{
	void *buf;
	unsigned int lockflags = 0;

	if (flags & MBV_LOCKFLAGS_READ) {
		lockflags |= DSLF_READ;
	}
	if (flags & MBV_LOCKFLAGS_WRITE) {
		lockflags |= DSLF_WRITE;
	}
	if (flags & MBV_LOCKFLAGS_FRONT) {
		LOG_PRINT_ERROR("Front buffer locking not supported!");
	}
	pthread_mutex_lock(&inst->lock);
	DFBCHECK(inst->surface->Lock(inst->surface, lockflags, &buf, pitch));

	if (!ALIGNED(buf, 4)) {
		DEBUG_PRINT("video-dfb", "Buffer not 32-bit aligned!");
	} else if (!ALIGNED(buf, 8)) {
		DEBUG_PRINT("video-dfb", "Buffer not 64-bit aligned!");
	}

	return buf;
}


/**
 * Unlock a previously locked surface.
 */
static void
surface_unlock(struct mbv_surface * const inst)
{
	DFBCHECK(inst->surface->Unlock(inst->surface));
	pthread_mutex_unlock(&inst->lock);
}


/**
 * Blits an RGB32 C buffer to the window's surface.
 */
static int
surface_blitbuf(
	struct mbv_surface * const inst,
	void *buf, int pitch, unsigned int flags, int width, int height, const int x, const int y)
{
	DFBSurfaceDescription dsc;
	static IDirectFBSurface *surface = NULL;

	assert(inst != NULL);
	assert(inst->surface != NULL);

	if (flags != MBV_BLITFLAGS_NONE) {
		LOG_VPRINT_ERROR("Invalid blit flags 0x%ux", flags);
	}

	dsc.width = width;
	dsc.height = height;
	dsc.flags = DSDESC_HEIGHT | DSDESC_WIDTH | DSDESC_PREALLOCATED | DSDESC_PIXELFORMAT;
	dsc.caps = DSCAPS_NONE;
	dsc.pixelformat = DSPF_RGB32;
	dsc.preallocated[0].data = buf;
	dsc.preallocated[0].pitch = pitch;
	dsc.preallocated[1].data = NULL;
	dsc.preallocated[1].pitch = 0;

	DFBCHECK(dfb->CreateSurface(dfb, &dsc, &surface));
	DFBCHECK(surface->SetBlittingFlags(surface, DSBLIT_NOFX));
	pthread_mutex_lock(&inst->lock);
	DFBCHECK(inst->surface->Blit(inst->surface, surface, NULL, x, y));
	pthread_mutex_unlock(&inst->lock);
	surface->Release(surface);
	return 0;
}


/**
 * Blit surface to surface.
 */
static int
surface_blit(
	struct mbv_surface * const dst,
	struct mbv_surface * const src,
	unsigned int flags, int x, int y)
{
	void *buf;
	int pitch, ret;

	/* DEBUG_PRINT("video-drm", "Entering surface_blit()"); */

	/* lock the src surface to get access to it's
	 * buffer */
	if ((buf = surface_lock(src, MBV_LOCKFLAGS_READ, &pitch)) == NULL) {
		LOG_PRINT_ERROR("Could not lock surface!");
		return -1;
	}

	/* blit the buffer */
	ret = surface_blitbuf(dst, buf, pitch, flags,
		src->rect.w, src->rect.h, x, y);
	surface_unlock(src);
	return ret;
}


/**
 * Creates a new window.
 */
static struct mbv_surface*
surface_new(
	struct mbv_surface * parent,
	const int x, const int y, int w, int h)
{
	struct mbv_surface *inst;

	if (parent == NULL) {
		parent = root;
	}

	/* first allocate the window structure */
	if ((inst = malloc(sizeof(struct mbv_surface))) == NULL) {
		LOG_PRINT_ERROR("surface_new() failed: Out of memory!");
		return NULL;
	}

	/* initialize window structure */
	inst->buf = NULL;
	inst->rect.x = x;
	inst->rect.y = y;
	inst->rect.w = w;
	inst->rect.h = h;
	inst->is_subwindow = (parent != root);

	if (pthread_mutex_init(&inst->lock, NULL) != 0) {
		fprintf(stderr, "video-dfb: Could not initialize mutex\n");
		free(inst);
		return NULL;
	}

	if (parent == root) {
		if (root == NULL) {
			DFBCHECK(layer->GetSurface(layer, &inst->surface));
		} else {
			int pitch;
			DFBSurfaceDescription dsc;

			/* allocate a properly aligned buffer */
			pitch = ((inst->rect.w * 4) + 15) & ~15;
			if ((errno = posix_memalign(&inst->buf, 16, pitch * inst->rect.h)) != 0) {
				LOG_VPRINT_ERROR("Could not allocated memory for surface: %s",
					strerror(errno));
				free(inst);
				return NULL;
			}

			dsc.flags = DSDESC_CAPS | DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PREALLOCATED | DSDESC_PIXELFORMAT;
			dsc.caps = DSCAPS_PREMULTIPLIED;
			dsc.width = w;
			dsc.height = h;
			dsc.pixelformat = DSPF_ARGB;
			dsc.preallocated[0].data = inst->buf;
			dsc.preallocated[0].pitch = pitch;
			dsc.preallocated[1].data = NULL;
			dsc.preallocated[1].pitch = 0;

			DFBCHECK(dfb->CreateSurface(dfb, &dsc, &inst->surface));
		}

		/* set basic drawing flags */
		DFBCHECK(inst->surface->SetBlittingFlags(
			inst->surface, DSBLIT_NOFX));
	} else {
		/* create the sub-window surface */
		DFBRectangle rect = { x, y, w, h };
		DFBCHECK(parent->surface->GetSubSurface(parent->surface, &rect, &inst->surface));
	}

	return inst;
}


/**
 * Flips the window surface into the root window
 */
static void
surface_update(struct mbv_surface * const inst,
	int blitflags, int update)
{
	/* DEBUG_VPRINT("video-dfb", "surface_update(0x%p)",
		inst); */

	assert(inst != NULL);

	if (inst->is_subwindow) {
		return;
	}

	if (inst == root) {
		/* if this is the root window simply flip the surface */
		pthread_mutex_lock(&inst->lock);
		DFBCHECK(inst->surface->Flip(inst->surface, NULL, DSFLIP_ONSYNC | DSFLIP_WAIT));
		pthread_mutex_unlock(&inst->lock);
	} else {
		/* if this is a regular window then blit it to the
		 * root window surface */
		DFBRectangle window_rect;
		window_rect.x = 0;
		window_rect.y = 0;
		window_rect.w = inst->rect.w;
		window_rect.h = inst->rect.h;
		if (blitflags & MBV_BLITFLAGS_ALPHABLEND) {
			DFBCHECK(root->surface->SetBlittingFlags(root->surface,
				DSBLIT_BLEND_ALPHACHANNEL));
		}
		DFBCHECK(root->surface->Blit(
			root->surface,
			inst->surface,
			&window_rect,
			inst->rect.x,
			inst->rect.y));
		if (blitflags & MBV_BLITFLAGS_ALPHABLEND) {
			DFBCHECK(root->surface->SetBlittingFlags(root->surface,
				DSBLIT_NOFX));
		}


		/* now blit the window to the front buffer. It would
		 * be nice if we could blit directly to the front buffer
		 * above but unfortunately this does not appear to be
		 * supported by DirectFB. We could Lock() the surface
		 * and write directly to it but we'll loose acceleration. */
		if (update) {
			DFBRegion region;
			region.x1 = inst->rect.x;
			region.y1 = inst->rect.y;
			region.x2 = inst->rect.x + inst->rect.w;
			region.y2 = inst->rect.y + inst->rect.h;
			pthread_mutex_lock(&root->lock);
			DFBCHECK(root->surface->Flip(root->surface,
				&region, DSFLIP_BLIT));
			pthread_mutex_unlock(&root->lock);
		}
	}
}


/**
 * Destroy a window
 */
static void
surface_destroy(struct mbv_surface *inst)
{
	assert(inst != NULL);

	/* release window surfaces */
	inst->surface->Release(inst->surface);

	/* free the surface buffer */
	if (inst->buf != NULL) {
		free(inst->buf);
	}

	/* free window object */
	free(inst);
}


/**
 * Callback by dfb to enumerate layers.
 */
static DFBEnumerationResult
enum_display_layers(DFBDisplayLayerID id, DFBDisplayLayerDescription desc, void *data)
{
	fprintf(stderr, "mbv: Found display layer %i\n", id);
	return DFENUM_OK;
}


static DFBEnumerationResult
mode_callback(int width, int height, int bpp, void *arg)
{
	(void) arg;
	DEBUG_VPRINT("video-dfb", "Video mode detected %ix%ix%i", width, height, bpp);
	return DFENUM_OK;
}


/**
 * Initialize video device.
 */
static struct mbv_surface *
init(int argc, char **argv, int * const w, int * const h)
{
	DFBCHECK(DirectFBInit(&argc, &argv));
	DFBCHECK(DirectFBCreate(&dfb));
	DFBCHECK(dfb->SetCooperativeLevel(dfb, DFSCL_NORMAL));
	DFBCHECK(dfb->EnumVideoModes(dfb, mode_callback, NULL));

	/* IDirectFBScreen does not return the correct size on SDL */
	DFBSurfaceDescription dsc;
	dsc.flags = DSDESC_CAPS;
	dsc.caps  = DSCAPS_PRIMARY | DSCAPS_PREMULTIPLIED | DSCAPS_DOUBLE | DSCAPS_FLIPPING;
	IDirectFBSurface *primary;
	DFBCHECK(dfb->CreateSurface(dfb, &dsc, &primary));
	DFBCHECK(primary->GetSize(primary, w, h));
	primary->Release(primary);

	/* enumerate display layers */
	DFBCHECK(dfb->EnumDisplayLayers(dfb, enum_display_layers, NULL));

	/* get primary layer */
	DFBCHECK(dfb->GetDisplayLayer(dfb, DLID_PRIMARY, &layer));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	DFBCHECK(layer->SetBackgroundColor(layer, 0x00, 0x00, 0x00, 0xff));
	DFBCHECK(layer->EnableCursor(layer, 0));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	
	/* create root surface */
	root = surface_new(NULL, 0, 0, *w, *h);
	if (root == NULL) {
		LOG_PRINT_ERROR("Could not create root surface for layer 0!");
		abort();
	}
	surface_update(root, MBV_BLITFLAGS_NONE, 1);

	//DFBCHECK(root->surface->SetPorterDuff(root->surface, DSPD_SRC_OVER));
	DFBCHECK(root->surface->SetSrcBlendFunction(root->surface, DSBF_SRCALPHA));
	DFBCHECK(root->surface->SetDstBlendFunction(root->surface, DSBF_INVSRCALPHA));

	/* print the pixel format of the root window */
	DFBSurfacePixelFormat pix_fmt;
	DFBCHECK(root->surface->GetPixelFormat(root->surface, &pix_fmt));
	DEBUG_VPRINT("video-dfb", "Root surface pixel format: %s",
		pixfmt_tostring(pix_fmt));

	return root;
}


/**
 * Destroy the directfb video driver.
 */
static void
__shutdown(void)
{
	surface_destroy(root);
	layer->Release(layer);
	dfb->Release(dfb);
}


void
mbv_dfb_initft(struct mbv_drv_funcs * const funcs)
{
	funcs->init = &init;
	funcs->surface_new = &surface_new;
	funcs->surface_lock = &surface_lock;
	funcs->surface_unlock = &surface_unlock;
	funcs->surface_blitbuf = &surface_blitbuf;
	funcs->surface_blit = &surface_blit;
	funcs->surface_update = &surface_update;
	funcs->surface_destroy = &surface_destroy;
	funcs->shutdown = &__shutdown;
}
