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


#ifndef __MB_VIDEO_DRV_H__
#define __MB_VIDEO_DRV_H__

/**
 * Abstract handle to a surface
 */
struct mbv_surface;
struct mbv_window;
struct mbv_drv_funcs;

#define MBV_BLITFLAGS_NONE		(0x0)
#define MBV_BLITFLAGS_FRONT		(0x1)
#define MBV_BLITFLAGS_ALPHABLEND	(0x2)

#define MBV_LOCKFLAGS_NONE	0
#define MBV_LOCKFLAGS_FRONT	1
#define MBV_LOCKFLAGS_READ	2
#define MBV_LOCKFLAGS_WRITE	4


/**
 * Initialize the video device and return
 * a pointer to the root surface.
 */
typedef struct mbv_surface *(*mbv_drv_init)(
	struct mbv_drv_funcs * const driver,
	int argc, char **argv, int * const w, int * const h);

/**
 * Create a new surface.
 */
typedef struct mbv_surface *(*mbv_drv_surface_new)(
	struct mbv_surface * parent,
	const int x, const int y, int w, int h);


/**
 * Lock a surface.
 */
typedef void *(*mbv_drv_surface_lock)(
	struct mbv_surface * const inst,
	unsigned int flags, int *pitch);


/**
 * Unlock a surface.
 */
typedef void (*mbv_drv_surface_unlock)(
	struct mbv_surface * const inst);


/**
 * Blit an RGB32 C buffer to the surface.
 */
typedef int (*mbv_drv_surface_blitbuf)(
	struct mbv_surface * const surface,
	void * buf, int pitch, unsigned int flags, int x, int y, int w, int h);


/**
 * Blit a surface unto another.
 */
typedef int (*mbv_drv_surface_blit)(
	struct mbv_surface * const dst,
	struct mbv_surface * const src,
	unsigned int flags, int x, int y);

/**
 * Scale and blit a surface unto another.
 */
typedef int (*mbv_drv_surface_scaleblit)(
	struct mbv_surface * const dst,
	struct mbv_surface * const src,
	unsigned int flags, int x, int y, int w, int h);

/**
 * Update a surface.
 */
typedef void (*mbv_drv_surface_update)(
	struct mbv_surface * const surface, int blitflags, int update);


/**
 * Destroys a surface and release all it's
 * resources.
 */
typedef void (*mbv_drv_surface_destroy)(
	struct mbv_surface * const surface);


/**
 * Shutdown the video device.
 */
typedef void (*mbv_drv_shutdown)(void);


/**
 * Video driver function table
 */
struct mbv_drv_funcs
{
	mbv_drv_init init;
	mbv_drv_surface_new surface_new;
	mbv_drv_surface_lock surface_lock;
	mbv_drv_surface_unlock surface_unlock;
	int (*surface_blitbuf)(
		struct mbv_surface * const surface,
		unsigned int pix_fmt, void **buf, int *pitch, unsigned int flags,
		int x, int y, int w, int h);
	mbv_drv_surface_blit surface_blit;
	mbv_drv_surface_scaleblit surface_scaleblit;
	mbv_drv_surface_update surface_update;
	mbv_drv_surface_destroy surface_destroy;
	int (*surface_doublebuffered)(const struct mbv_surface * const);
	mbv_drv_shutdown shutdown;
};

#endif
