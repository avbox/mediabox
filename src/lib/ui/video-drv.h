#ifndef __MB_VIDEO_DRV_H__
#define __MB_VIDEO_DRV_H__

/**
 * Abstract handle to a surface
 */
struct mbv_surface;
struct mbv_window;

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
	mbv_drv_surface_blitbuf surface_blitbuf;
	mbv_drv_surface_blit surface_blit;
	mbv_drv_surface_update surface_update;
	mbv_drv_surface_destroy surface_destroy;
	mbv_drv_shutdown shutdown;
};

#endif
