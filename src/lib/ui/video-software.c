/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifdef HAVE_CONFIG_H
#       include <libavbox/config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LOG_MODULE "video-software"

#include <libavbox/avbox.h>


/* define to 1 if swap_buffers will never
 * be NULL */
#define ALWAYS_SWAP	1


struct mbv_surface
{
	struct mbv_surface *real;
	struct mbv_surface *parent;
	uint8_t *pixels;
	int pitch;
	uint32_t w;
	uint32_t h;
	uint32_t x;
	uint32_t y;
	uint32_t realx;
	uint32_t realy;
};


/* software renderer */
static struct mbv_surface *display_surface;
static struct mbv_surface *root_surface;
static void (*wait_for_vsync)(void);
static void (*swap_buffers)(void);


static int
surface_doublebuffered(const struct mbv_surface * const surface)
{
	return (surface == root_surface);
}


static struct mbv_surface *
surface_new(struct mbv_surface *parent,
	const int x, const int y, const int w, const int h)
{
	struct mbv_surface *inst;

	/* DEBUG_PRINT("video-drm", "Entering surface_new()"); */

	/* allocate memory for the surface object */
	if ((inst = malloc(sizeof(struct mbv_surface))) == NULL) {
		LOG_VPRINT_ERROR("Could not create surface: %s",
			strerror(errno));
		return NULL;
	}

	inst->w = w;
	inst->h = h;
	inst->x = x;
	inst->y = y;

	if (parent == NULL) {
		inst->parent = NULL;
		inst->real = inst;
		inst->realx = 0;
		inst->realy = 0;
		inst->pitch = ((w * 4) + 15) & ~15;

		/* allocate a buffer for surface in system memory */
		if ((errno = posix_memalign((void*)&inst->pixels, 16, inst->pitch * h)) == -1) {
			LOG_VPRINT_ERROR("Could not allocate surface buffer: %s",
				strerror(errno));
			free(inst);
			return NULL;
		}
	} else {
		inst->parent = parent;
		inst->real = parent->real;
		inst->realx = parent->realx + inst->x;
		inst->realy = parent->realy + inst->y;
		inst->pitch = parent->pitch;
		inst->pixels = ((uint8_t*)parent->pixels) + (inst->pitch * inst->y) + inst->x;
	}

	return inst;
}


static void *
surface_lock(struct mbv_surface * const inst,
	unsigned int flags, int *pitch)
{
	*pitch = inst->real->pitch;

	if (flags & MBV_LOCKFLAGS_FRONT) {
		ASSERT(inst == root_surface);	/* only the root surface is double buffered */
		return display_surface->pixels;
	}

	return inst->pixels;
}


static void
surface_unlock(struct mbv_surface * const inst)
{
}


static int
surface_blitbuf(struct mbv_surface * const surface,
	unsigned int pix_fmt, void **buf, int *pitch, unsigned int flags, int w, int h, int x, int y)
{
	switch (pix_fmt) {
	case AVBOX_PIXFMT_YUV420P:
	{
		int dstpitch;
		uint8_t *surface_buf;
		struct SwsContext *swscale;

		if ((swscale = sws_getContext(
			w, h, avbox_pixfmt_to_libav(pix_fmt),
			w, h, AV_PIX_FMT_BGRA,
			SWS_FAST_BILINEAR,
			NULL, NULL, NULL)) == NULL) {
			LOG_PRINT_ERROR("Could not create swscale context!");
			return -1;
		}

		if ((surface_buf = surface_lock(surface, MBV_LOCKFLAGS_WRITE, &dstpitch)) == NULL) {
			sws_freeContext(swscale);
			return -1;
		}

		surface_buf += dstpitch * y;
		surface_buf += x * 4;
		sws_scale(swscale, (const uint8_t**) buf,
			pitch, 0, h, &surface_buf, &dstpitch);
		surface_unlock(surface);
		sws_freeContext(swscale);
		break;
	}
	case AVBOX_PIXFMT_BGRA:
	{
		int dst_pitch;
		uint8_t *dst;
		unsigned int lockflags = MBV_LOCKFLAGS_WRITE;

		if (flags & MBV_BLITFLAGS_FRONT) {
			lockflags |= MBV_LOCKFLAGS_FRONT;
		}

		dst = surface_lock(surface, lockflags, &dst_pitch);
		if (dst == NULL) {
			LOG_PRINT_ERROR("Could not lock surface");
			surface_unlock(surface);
			return -1;
		}

		dst += y * dst_pitch;

		if (0 && x == 0 && *pitch == dst_pitch && pitch[0] == (w * 4)) {
			/* both surfaces are in contiguos memory so we
			 * can just memcpy() the whole thing */
			memcpy(dst, buf, *pitch * h);
		} else {
			const uint8_t *src = *buf;
			const uint8_t * const end = src + (pitch[0] * h);
			const int stride_sz = w * 4;
			for (dst += x * 4; src < end; dst += dst_pitch, src += pitch[0]) {
				memcpy(dst, src, stride_sz);
			}
		}

		surface_unlock(surface);
		break;
	}
	default:
		abort();
	}

	return 0;
}


static int
surface_blit(struct mbv_surface * const dst,
	struct mbv_surface * const src,
	unsigned int flags, int x, int y)
{
	void *buf;
	int pitch, ret;

	/* lock the src surface to get access to it's
	 * buffer */
	if ((buf = surface_lock(src, MBV_LOCKFLAGS_READ, &pitch)) == NULL) {
		LOG_PRINT_ERROR("Could not lock surface!");
		return -1;
	}

	/* blit the buffer */
	ret = surface_blitbuf(dst, AVBOX_PIXFMT_BGRA, &buf, &pitch, flags,
		src->w, src->h, x, y);
	surface_unlock(src);
	return ret;
}


static void
surface_update(struct mbv_surface * const surface,
	int blitflags, const int update)
{
	ASSERT(surface != NULL);
	(void) blitflags;

	if (surface->real != surface) {
		return;
	}

	if (surface == root_surface) {
		if (surface == root_surface) {
			wait_for_vsync();
		}
		if (ALWAYS_SWAP || swap_buffers != NULL) {
			/* if the driver supports page flipping just
			 * swap buffers and request a page flip */
			uint8_t * const tmp = root_surface->pixels;
			root_surface->pixels = display_surface->pixels;
			display_surface->pixels = tmp;
			swap_buffers();
		} else {
			/* no page flipping support so we need to copy our
			 * back buffer to the framebuffer manually */
			memcpy(display_surface->pixels, root_surface->pixels,
				display_surface->pitch * display_surface->h);
		}
	} else {
		if (update) {
			/* blit the surface directly to the screen */
			surface_blit(display_surface, surface, blitflags,
				surface->x, surface->y);
		} else {
			/* blit the surface to the screen backbuffer */
			surface_blit(root_surface, surface, blitflags,
				surface->x, surface->y);
		}
	}
}


static void
surface_destroy(struct mbv_surface *inst)
{
	ASSERT(inst != NULL);
	ASSERT(inst->pixels != NULL);
	if (inst->parent == NULL) {
		free(inst->pixels);
	}
	free(inst);
}


/**
 * Initialize the software renderer
 */
struct mbv_surface *
avbox_video_softinit(struct mbv_drv_funcs * const funcs,
	uint8_t *front_pixels, uint8_t *back_pixels, const int w, const int h, const int pitch,
	void (*wait_for_vsync_fn)(void), void (*swap_buffers_fn)(void))
{
	DEBUG_PRINT(LOG_MODULE, "Initializing software renderer");

	if ((display_surface = malloc(sizeof(struct mbv_surface))) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}

	if ((root_surface = malloc(sizeof(struct mbv_surface))) == NULL) {
		ASSERT(errno == ENOMEM);
		free(display_surface);
		return NULL;
	}

	memset(root_surface, 0, sizeof(struct mbv_surface));
	root_surface->parent = NULL;
	root_surface->real = root_surface;
	root_surface->w = w;
	root_surface->h = h;

	if (back_pixels == NULL) {
		root_surface->pitch = w * 4;
		/* allocate pixel storage for the back surface */
		if ((root_surface->pixels = malloc(root_surface->pitch * h)) == NULL) {
			ASSERT(errno == ENOMEM);
			free(display_surface);
			free(root_surface);
			return NULL;
		}
	} else {
		root_surface->pitch = pitch;
		root_surface->pixels = back_pixels;
	}

	/* initialize the display surface */
	memset(display_surface, 0, sizeof(struct mbv_surface));
	display_surface->pixels = front_pixels;
	display_surface->real = display_surface;
	display_surface->w = w;
	display_surface->h = h;
	display_surface->pitch = pitch;

	/* initialize function table */
	funcs->surface_new = &surface_new;
	funcs->surface_lock = &surface_lock;
	funcs->surface_unlock = &surface_unlock;
	funcs->surface_blitbuf = &surface_blitbuf;
	funcs->surface_blit = &surface_blit;
	funcs->surface_scaleblit = NULL;
	funcs->surface_update = &surface_update;
	funcs->surface_doublebuffered = &surface_doublebuffered;
	funcs->surface_destroy = &surface_destroy;

	wait_for_vsync = wait_for_vsync_fn;
	swap_buffers = swap_buffers_fn;

	return root_surface;
}
