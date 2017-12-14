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
#	include "../../config.h"
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
#include <pthread.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define LOG_MODULE "video-drm"

#include "../log.h"
#include "../debug.h"
#include "../linkedlist.h"
#include "video-drv.h"
#include "video-software.h"

#ifdef ENABLE_OPENGL
#	include <EGL/egl.h>
#	include <EGL/eglext.h>
#	include <gbm.h>
#	include "video-opengl.h"
#endif


struct mbv_drm_dev;


struct avbox_drm_surface
{
	uint32_t dbo;
	uint32_t fbo;
	int pitch;
	uint8_t *pixels;
};


LISTABLE_STRUCT(mbv_drm_dev,
	int fd;
	uint32_t conn;
	uint32_t crtc;
	uint32_t w;
	uint32_t h;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	struct avbox_drm_surface *front;
	struct avbox_drm_surface *back;
);


static LIST devices;
static struct mbv_drm_dev *default_dev = NULL;


#ifdef ENABLE_OPENGL
static struct gbm_device *gbm_dev;
static EGLDisplay egl_display;
static EGLContext egl_ctx;
static EGLSurface egl_surface;
static struct gbm_surface *gbm_surface;
static int egl_enabled = 0;
static struct gbm_bo *bo = NULL;
static int waiting_for_flip = 0;
static fd_set flip_fds;
#endif


static void
avbox_drm_free_framebuffer(struct mbv_drm_dev *dev)
{
	struct drm_mode_destroy_dumb dreq;

	/* delete framebuffers */
	drmModeRmFB(dev->fd, dev->front->fbo);
	drmModeRmFB(dev->fd, dev->back->fbo);

	/* delete dumb buffers */
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = dev->front->dbo;
	drmIoctl(default_dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = dev->back->dbo;
	drmIoctl(default_dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

	free(dev->front);
	free(dev->back);
}


static int
avbox_drm_create_framebuffer(struct mbv_drm_dev * const dev,
	struct avbox_drm_surface **surface)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq = { 0 };
	struct drm_mode_destroy_dumb dreq;
	int sz;
	int ret;

	DEBUG_PRINT("video-drm", "Creating framebuffer surface");

	ASSERT(dev != NULL);

	if (((*surface) = malloc(sizeof(struct avbox_drm_surface))) == NULL) {
		ASSERT(errno = ENOMEM);
		return -1;
	}

	/* create dumb back buffer */
	DEBUG_PRINT(LOG_MODULE, "Creating dumb buffer");
	memset(&creq, 0, sizeof(struct drm_mode_create_dumb));
	creq.width = dev->w;
	creq.height = dev->h;
	creq.bpp = 32;
	ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		LOG_VPRINT_ERROR("Cannot create dumb buffer (%d) %s",
			errno, strerror(errno));
		return -errno;
	}

	ASSERT(creq.pitch <= INT_MAX);
	(*surface)->dbo = creq.handle;
	(*surface)->pitch = creq.pitch;
	sz = creq.size;

	DEBUG_VPRINT("video-drm", "Dumb buffer handle: 0x%x", (*surface)->dbo);
	DEBUG_VPRINT("video-drm", "Dumb buffer pitch: %i", (*surface)->pitch);
	DEBUG_VPRINT("video-drm", "Dumb buffer size: %u", sz);
	
	/* create framebuffer object */
	DEBUG_PRINT("video-drm", "Creating framebuffer objects");
	ret = drmModeAddFB(dev->fd, dev->w, dev->h, 24, 32,
		(*surface)->pitch, (*surface)->dbo, &(*surface)->fbo);
	if (ret) {
		LOG_VPRINT_ERROR("Cannot create framebuffer (%d) %s",
			errno, strerror(errno));
		ret = -errno;
		goto err_destroy;
	}

	DEBUG_VPRINT("video-drm", "Framebuffer object: 0x%x",
		dev->front->fbo);

	/* prepare buffer for memory mapping */
	/* DEBUG_PRINT("video-drm", "Preparing to map surface"); */
	mreq.handle = (*surface)->dbo;
	ASSERT(mreq.offset == 0);
	if (drmIoctl(dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
		LOG_VPRINT_ERROR("Cannot map dumb buffer (%d) %s",
			errno, strerror(errno));
		goto err_fb;
	}

	/* perform actual memory mapping */
	(*surface)->pixels = mmap(0, sz,
		PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, mreq.offset);
	if ((*surface)->pixels == MAP_FAILED) {
		LOG_VPRINT_ERROR("Cannot mmap dumb buffer (%d) %s",
			errno, strerror(errno));
		goto err_fb;
	}

	DEBUG_VPRINT("video-drm", "Framebuffer mapped at 0x%p", (*surface)->pixels);

	/* clear the framebuffer to 0 */
	DEBUG_PRINT("video-drm", "Clearing framebuffers");
	memset((*surface)->pixels, 0, sz);


	DEBUG_PRINT("video-drm", "Framebuffers created");
	return 0;

err_fb:
	drmModeRmFB(dev->fd, (*surface)->fbo);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = (*surface)->dbo;
	drmIoctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}


static int
mbv_drm_findcrtc(struct mbv_drm_dev *dev,
	drmModeRes *res, drmModeConnector *conn)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	int32_t crtc;
	struct mbv_drm_dev *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id) {
		enc = drmModeGetEncoder(dev->fd, conn->encoder_id);
	} else {
		enc = NULL;
	}

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			LIST_FOREACH(struct mbv_drm_dev*, iter, &devices) {
				if (iter->crtc == crtc) {
					crtc = -1;
					break;
				}
			}

			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(dev->fd, conn->encoders[i]);
		if (!enc) {
			LOG_VPRINT_ERROR("Cannot retrieve encoder %u:%u (%d): %s",
				i, conn->encoders[i], errno, strerror(errno));
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j))) {
				continue;
			}

			/* check that no other device already uses this CRTC */
			crtc = res->crtcs[j];
			LIST_FOREACH(struct mbv_drm_dev*, iter, &devices) {
				if (iter->crtc == crtc) {
					crtc = -1;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	LOG_VPRINT_ERROR("Cannot find suitable CRTC for connector %u",
		conn->connector_id);
	return -ENOENT;
}


static int
mbv_drm_setupdev(struct mbv_drm_dev *dev,
	drmModeRes *res, drmModeConnector *conn, int mode_index)
{
	int ret;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		DEBUG_VPRINT("video-drm", "Ignoring unused connector %u",
			conn->connector_id);
		return -ENOENT;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		DEBUG_VPRINT("video-drm", "No valid mode for connector %u",
			conn->connector_id);
		return -EFAULT;
	}

	/* copy the mode information into our device structure */
	memcpy(&dev->mode, &conn->modes[mode_index], sizeof(dev->mode));

	DEBUG_VPRINT("video-drm", "Mode for connector %u is %ux%u",
		conn->connector_id, conn->modes[mode_index].hdisplay,
		conn->modes[mode_index].vdisplay);

	/* find a crtc for this connector */
	if ((ret = mbv_drm_findcrtc(dev, res, conn)) != 0) {
		LOG_VPRINT_ERROR("No valid CRTC for connector %u",
			conn->connector_id);
		return ret;
	}

	DEBUG_VPRINT("video-drm", "Device CRTC = 0x%x",
		dev->crtc);

	dev->w = conn->modes[mode_index].hdisplay;
	dev->h = conn->modes[mode_index].vdisplay;

	/* save crtc */
	dev->saved_crtc = drmModeGetCrtc(dev->fd, dev->crtc);

	DEBUG_VPRINT("video-drm", "Connector %u ready.",
		conn->connector_id);

	return 0;
}


static int
mbv_drm_prepare(int fd, int mode_index)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct mbv_drm_dev *dev;
	int ret;

	DEBUG_PRINT("video-drm", "Initializing modesetting devices");

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		LOG_VPRINT_ERROR("Cannot retrieve DRM resources (%d): %s",
			errno, strerror(errno));
		return -errno;
	}

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			LOG_VPRINT_ERROR("Cannot retrieve DRM connector %u:%u (%d): %s",
				i, res->connectors[i], errno, strerror(errno));
			continue;
		}

		/* list supported modes */
		for (ret = 0; ret < conn->count_modes; ret++) {
			DEBUG_VPRINT("video-drm", "Mode: %i %s %ix%i@%i vscan=%i htotal=%i vtotal=%i",
				ret, conn->modes[i].name, conn->modes[i].hdisplay, conn->modes[i].vdisplay,
				conn->modes[i].vrefresh, conn->modes[i].vscan,
				conn->modes[i].htotal, conn->modes[i].vtotal);
		}

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->conn = conn->connector_id;
		dev->fd = fd;

		/* call helper function to prepare this connector */
		ret = mbv_drm_setupdev(dev, res, conn, mode_index);
		if (ret) {
			if (ret != -ENOENT) {
				errno = -ret;
				LOG_VPRINT_ERROR("Cannot setup device for connector %u:%u (%d): %s",
					i, res->connectors[i], errno, strerror(errno));
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		if (default_dev == NULL) {
			default_dev = dev;
		}

		/* free connector data and link device into global list */
		drmModeFreeConnector(conn);
		LIST_ADD(&devices, dev);
	}

	/* free resources again */
	drmModeFreeResources(res);

	DEBUG_PRINT("video-drm", "Modesetting initialized!");

	return 0;
}


static void
avbox_drm_wait_for_vsync(void)
{
	drmVBlank vb;
	vb.request.type = DRM_VBLANK_RELATIVE;
	vb.request.sequence = 1;
	vb.request.signal = 0;
	drmWaitVBlank(default_dev->fd, &vb);
}


static inline void
avbox_drm_swap_buffers(void)
{
	struct avbox_drm_surface * const tmp = default_dev->front;
	default_dev->front = default_dev->back;
	default_dev->back = tmp;

	/* swap the front buffer in */
	if (drmModeSetCrtc(default_dev->fd, default_dev->crtc,
		default_dev->front->fbo, 0, 0, &default_dev->conn, 1, &default_dev->mode)) {
		LOG_PRINT_ERROR("Could not swap buffers");
	}
	DEBUG_PRINT(LOG_MODULE, "Swapped buffers");
}


#ifdef ENABLE_OPENGL


void
avbox_drm_egl_fb_destroy_callback(struct gbm_bo * const bo, void *data)
{
	drmModeRmFB(default_dev->fd, *((uint32_t*)data));
	free(data);
}


static int
avbox_drm_egl_bo_framebuffer(struct gbm_bo * const bo, uint32_t *fbo)
{
	int ret;
	uint32_t *fbo_mem;

	/* if the bo already has a fb return it */
	if ((fbo_mem = gbm_bo_get_user_data(bo)) != NULL) {
		*fbo = *fbo_mem;
		return 0;
	}

	/* allocate memory for fbo */
	if ((fbo_mem = malloc(sizeof(uint32_t))) == NULL) {
		ASSERT(errno == ENOMEM);
		return -1;
	}

	/* create a framebuffer for the buffer object */
	if ((ret = drmModeAddFB(default_dev->fd, default_dev->w, default_dev->h, 24, 32,
		gbm_bo_get_stride(bo), gbm_bo_get_handle(bo).u32, fbo_mem)) != 0) {
		LOG_VPRINT_ERROR("Could not create GBM fb: %s",
			strerror(ret));
		return -1;
	}

	/* save for later and return */
	gbm_bo_set_user_data(bo, fbo_mem, avbox_drm_egl_fb_destroy_callback);
	*fbo = *fbo_mem;
	return 0;

}


static void
avbox_drm_page_flip_handler(int fd, unsigned int frame,
                  unsigned int sec, unsigned int usec, void *data)
{
        (void)fd, (void)frame, (void)sec, (void)usec;
        int *waiting_for_flip = data;
        *waiting_for_flip = 0;
}


static void
avbox_drm_egl_swap_buffers(void)
{
	uint32_t fbo;

	if (bo != NULL) {
		gbm_surface_release_buffer(gbm_surface, bo);
	}

	/* get the buffer object for the front buffer */
	eglSwapBuffers(egl_display, egl_surface);
	if ((bo = gbm_surface_lock_front_buffer(gbm_surface)) == NULL) {
		LOG_PRINT_ERROR("Could not get the surface's buffer object");
		return;
	}

	/* get the bo's framebuffer */
	if (avbox_drm_egl_bo_framebuffer(bo, &fbo) == -1) {
		LOG_PRINT_ERROR("Could not get framebuffer object!");
		gbm_surface_release_buffer(gbm_surface, bo);
		return;
	}

	/* wait until the last flip completes */
	while (waiting_for_flip) {
		drmEventContext evctx = { .version = 2, .page_flip_handler = avbox_drm_page_flip_handler };
		if (select(default_dev->fd + 1, &flip_fds, NULL, NULL, NULL) > 0) {
			drmHandleEvent(default_dev->fd, &evctx);
		}
	}

	/* flip the new buffer in */
	waiting_for_flip = 1;
	if (drmModePageFlip(default_dev->fd,
		default_dev->crtc,
		fbo, DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip) != 0) {
		LOG_PRINT_ERROR("Could not set EGL framebuffer");
	}
}


static int
avbox_drm_create_egl_framebuffer(
	EGLDisplay egl_display,
	struct mbv_drm_dev * const dev, EGLConfig *cfg)
{
	const EGLint surfaceAttribs[] = {
		EGL_RENDER_BUFFER,
		EGL_BACK_BUFFER,
		EGL_NONE
	};
	uint32_t fbo;

	FD_ZERO(&flip_fds);
	FD_SET(default_dev->fd, &flip_fds);

	gbm_surface = gbm_surface_create(gbm_dev, default_dev->w, default_dev->h,
		GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (gbm_surface == NULL) {
		LOG_PRINT_ERROR("Could not create GBM surface!");
		gbm_device_destroy(gbm_dev);
		eglTerminate(egl_display);
		return -1;
	}

	if ((egl_surface = eglCreateWindowSurface(egl_display, *cfg,
		(EGLNativeWindowType) gbm_surface, surfaceAttribs)) == EGL_NO_SURFACE) {
		LOG_PRINT_ERROR("Could not create window surface");
		gbm_surface_destroy(gbm_surface);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
		return -1;
	}

	if (eglSurfaceAttrib(egl_display, egl_surface, EGL_SWAP_BEHAVIOR,
		EGL_BUFFER_PRESERVED) == EGL_FALSE) {
		LOG_PRINT_ERROR("COuld not set surface attributes");
	}

	/* swap the front surface in */
	if (eglMakeCurrent(egl_display,
		egl_surface, egl_surface, egl_ctx) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not make GL context current!");
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
		return -1;
	}

	avbox_drm_egl_swap_buffers();
	/* get the buffer object for the front buffer */
	eglSwapBuffers(egl_display, egl_surface);
	if ((bo = gbm_surface_lock_front_buffer(gbm_surface)) == NULL) {
		LOG_PRINT_ERROR("Could not get the surface's buffer object");
		return -1;
	}
	if (avbox_drm_egl_bo_framebuffer(bo, &fbo) == -1) {
		LOG_PRINT_ERROR("Could not get framebuffer object!");
		gbm_surface_release_buffer(gbm_surface, bo);
		return -1;
	}

	/* flip it */
	if (drmModeSetCrtc(default_dev->fd,
		default_dev->crtc,
		fbo, 0, 0,
		&default_dev->conn, 1,
		&default_dev->mode)) {
		LOG_PRINT_ERROR("Could not set EGL framebuffer");
	}

	return 0;
}


static struct mbv_surface *
avbox_drm_egl_init(struct mbv_drv_funcs * const driver,
	const int fd, const int w, const int h)
{
	const EGLint configAttribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_BLUE_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_RED_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_NONE
	};

	EGLint major, minor, n_configs, err;
	EGLConfig cfg;
	void *gl_surface;

	/* create a framebuffer bo */
	gbm_dev = gbm_create_device(fd);

	#if 1
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
		get_platform_display = (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
	assert(get_platform_display != NULL);


	#define EGL_PLATFORM_GBM_KHR              0x31D7

	if ((egl_display = get_platform_display(EGL_PLATFORM_GBM_KHR, (void*) gbm_dev, NULL)) == EGL_NO_DISPLAY) {
		LOG_PRINT_ERROR("Could not create EGL display!");
		gbm_device_destroy(gbm_dev);
		goto end;
	}
	#else
	/* get display */
	if ((egl_display = eglGetDisplay((void*) gbm_dev)) == EGL_NO_DISPLAY) {
		LOG_PRINT_ERROR("Could not create EGL display!");
		goto end;
	}
	#endif

	if (eglInitialize(egl_display, &major, &minor) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not initialize EGL display!");
		gbm_device_destroy(gbm_dev);
		goto end;
	}
	if (eglChooseConfig(egl_display, configAttribs, &cfg, 1, &n_configs) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not find EGL config!");
		gbm_device_destroy(gbm_dev);
		eglTerminate(egl_display);
		goto end;
	}
	if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not bind OpenGL API!");
		gbm_device_destroy(gbm_dev);
		eglTerminate(egl_display);
		goto end;
	}
	if ((egl_ctx = eglCreateContext(egl_display, cfg, EGL_NO_CONTEXT, NULL)) == EGL_NO_CONTEXT) {
		LOG_PRINT_ERROR("Could not create egl context!");
		if ((err = eglGetError()) != EGL_SUCCESS) {
			LOG_VPRINT_ERROR("GL error: 0x%x", err);
		}
		gbm_device_destroy(gbm_dev);
		eglTerminate(egl_display);
		goto end;
	}

	/* create a DRM framebuffer from the bo */
	if (avbox_drm_create_egl_framebuffer(egl_display, default_dev, &cfg) == -1) {
		LOG_PRINT_ERROR("Could not create EGL framebuffer (front)!");
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
		goto end;
	}

	/* now that we have everything setup we can attempt to
	 * start the GL driver */
	if ((gl_surface = avbox_video_glinit(driver,
		w, h, avbox_drm_egl_swap_buffers)) != NULL) {
		egl_enabled = 1;
		return gl_surface;
	} else {
		LOG_PRINT_ERROR("Could not initialize GL driver! Using software driver.");
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
	}
end:
	return NULL;
}
#endif


static struct mbv_surface *
init(struct mbv_drv_funcs * const driver,
	int argc, char **argv, int * const w, int * const h)
{
	int i, ret, fd = -1;
	int mode_index = 0, accel = 1;
	const char *card = "/dev/dri/card0";
	uint64_t has_dumb;
	struct mbv_surface *root;

	ASSERT(w != NULL);
	ASSERT(h != NULL);

	DEBUG_VPRINT("video-drm", "Using card '%s'",
		card);

	for (i = 0; i < argc; i++) {
		if (!strncmp("--video:mode_index=", argv[i], 19)) {
			mode_index = atoi(&argv[i][18]);
			DEBUG_VPRINT("video-drm", "Specified mode index: %i",
				mode_index);

		} else if (!strncmp(argv[i], "--no-accel", 10)) {
			accel = 0;
		}
	}

	LIST_INIT(&devices);

	/* open the DRM device */
	fd = open(card, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		LOG_VPRINT_ERROR("Cannot open '%s': %s",
			card, strerror(errno));
		goto end;
	}

	/* prepare all connectors and CRTCs */
	ret = mbv_drm_prepare(fd, mode_index);
	if (ret) {
		goto end;
	}

	*w = default_dev->w;
	*h = default_dev->h;
	ret = 0;

#ifdef ENABLE_OPENGL
	if (accel) {
		/* initialize an EGL framebuffer */
		if ((root = avbox_drm_egl_init(driver, default_dev->fd,
			default_dev->w, default_dev->h)) == NULL) {
			LOG_PRINT_ERROR("Could not initialize EGL framebuffer");
		} else {
			return root;
		}
	}
#endif

	/* At this point either GL initialization failed or we
	 * have no GL support built in so we need to initialize
	 * the software renderer */

	/* check that the device has dumb buffer support */
	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
		LOG_VPRINT_ERROR("DRM device '%s' does not support dumb buffers!",
			card);
		ret = -EOPNOTSUPP;
		goto end;
	}

	/* create a front buffer */
	if ((ret = avbox_drm_create_framebuffer(default_dev, &default_dev->front)) != 0) {
		LOG_VPRINT_ERROR("Cannot create framebuffers for connector %u",
			default_dev->conn);
		goto end;
	}

	/* create back buffer */
	if ((ret = avbox_drm_create_framebuffer(default_dev, &default_dev->back)) != 0) {
		LOG_VPRINT_ERROR("Cannot create framebuffers for connector %u",
			default_dev->conn);
		goto end;
	}

	/* swap the front buffer in */
	if (drmModeSetCrtc(default_dev->fd, default_dev->crtc,
		default_dev->front->fbo, 0, 0, &default_dev->conn, 1, &default_dev->mode)) {
		LOG_PRINT_ERROR("Could not set EGL framebuffer");
		goto end;
	}

	/* initialize software renderer */
	if ((root = avbox_video_softinit(driver,
		default_dev->front->pixels, default_dev->back->pixels,
		default_dev->w, default_dev->h, default_dev->front->pitch,
		avbox_drm_wait_for_vsync, avbox_drm_swap_buffers)) == NULL) {
		LOG_PRINT_ERROR("Could not initialize software driver!");
		goto end;
	}

	return root;

end:
	errno = -ret;
	LOG_VPRINT_ERROR("Modesetting failed with error %d: %s",
		errno, strerror(errno));
	if (fd != -1) {
		close(fd);
	}
	return NULL;
}


/*
 * This cleans up all the devices we created during
 * modeset_prepare(). It resets the CRTCs to their saved
 * states and deallocates all memory.
 */
static void
shutdown()
{
	struct mbv_drm_dev *iter;

#ifdef ENABLE_OPENGL
	if (egl_enabled) {
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
	}
#endif

	LIST_FOREACH_SAFE(struct mbv_drm_dev*, iter, &devices, {
		/* remove from global list */
		LIST_REMOVE(iter);

		/* restore saved CRTC configuration */
		drmModeSetCrtc(iter->fd,
			       iter->saved_crtc->crtc_id,
			       iter->saved_crtc->buffer_id,
			       iter->saved_crtc->x,
			       iter->saved_crtc->y,
			       &iter->conn,
			       1,
			       &iter->saved_crtc->mode);
		drmModeFreeCrtc(iter->saved_crtc);

		if (iter == default_dev && !egl_enabled) {
			avbox_drm_free_framebuffer(iter);
		}

		/* free allocated memory */
		free(iter);
	});
}


void
mbv_drm_initft(struct mbv_drv_funcs * const funcs)
{
	funcs->init = &init;
	funcs->shutdown = &shutdown;
}
