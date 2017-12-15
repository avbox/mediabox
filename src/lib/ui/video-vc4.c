#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <bcm_host.h>
#include <EGL/eglext_brcm.h>

#define LOG_MODULE "video-vc4"

#include "../debug.h"
#include "../log.h"
#include "../thread.h"
#include "video-drv.h"
#include "video-opengl.h"


/* VC4 driver */
static EGLDisplay display;
static EGLSurface surface;
static EGLContext ctx;
static DISPMANX_DISPLAY_HANDLE_T dispman_display;
static DISPMANX_ELEMENT_HANDLE_T dispman_element;
static EGL_DISPMANX_WINDOW_T nativewindow;
static DISPMANX_UPDATE_HANDLE_T dispman_update;


static void
swap_buffers(void)
{
	eglSwapBuffers(display, surface);
}


/**
 * Assigns an MMAL buffer to a GL texture.
 */
int
avbox_video_vc4_mmal2texture(void *buf, GLuint texture)
{
	static EGLImageKHR *image = EGL_NO_IMAGE_KHR;

	if (image != EGL_NO_IMAGE_KHR) {
		eglDestroyImageKHR(display, image);
		image = EGL_NO_IMAGE_KHR;
	}

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);

	/* create an EGL image */
	if ((image = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_IMAGE_BRCM_MULTIMEDIA,
		(EGLClientBuffer*) buf, NULL)) == EGL_NO_IMAGE_KHR) {
		LOG_PRINT_ERROR("Could not create EGL image!");
		return -1;
	}

	/* assign it to the texture */
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

	return 0;
}


/**
 * Initialize the X11 driver.
 */
static struct mbv_surface *
init(struct mbv_drv_funcs * const driver, int argc, char **argv, int * const w, int * const h)
{
	struct mbv_surface *surf = NULL;

	static const EGLint attribute_list[] =
	{
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_NONE
	};
   
	static const EGLint context_attributes[] =
	{
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	VC_RECT_T dst_rect;
	VC_RECT_T src_rect;

	EGLConfig config;
	EGLint n_config;
	uint32_t screen_width, screen_height;

	bcm_host_init();

	/* get an EGL display connection */
	if ((display = eglGetDisplay(EGL_DEFAULT_DISPLAY)) == EGL_NO_DISPLAY) {
		LOG_PRINT_ERROR("Could not get EGL display!");
		return NULL;
	}

	// initialize the EGL display connection
	if (eglInitialize(display, NULL, NULL) == EGL_FALSE) {
		LOG_PRINT_ERROR("COuld not initialize EGL!");
		return NULL;
	}

	/* get an appropriate EGL frame buffer configuration */
	if (eglChooseConfig(display, attribute_list, &config, 1, &n_config) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not choose EGL config");
		return NULL;
	}

	if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not bind GLESv2 API");
		return NULL;
	}

	/* create an EGL rendering context */
	if ((ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes)) == EGL_NO_CONTEXT) {
		LOG_PRINT_ERROR("Could not create EGL context!");
		return NULL;
	}

	/* create an EGL window surface */
	if (graphics_get_display_size(0 /* LCD */, &screen_width, &screen_height) < 0) {
		LOG_PRINT_ERROR("Could not get display size");
		return NULL;
	}


	dst_rect.x = 0;
	dst_rect.y = 0;
	dst_rect.width = screen_width;
	dst_rect.height = screen_height;
	src_rect.x = 0;
	src_rect.y = 0;
	src_rect.width = screen_width << 16;
	src_rect.height = screen_height << 16;        
	*w = screen_width;
	*h = screen_height;

	dispman_display = vc_dispmanx_display_open( 0 /* LCD */);
	dispman_update = vc_dispmanx_update_start( 0 );

	dispman_element = vc_dispmanx_element_add(dispman_update, dispman_display,
		0/*layer*/, &dst_rect, 0/*src*/,
		&src_rect, DISPMANX_PROTECTION_NONE, 0 /*alpha*/, 0/*clamp*/, 0/*transform*/);

	nativewindow.element = dispman_element;
	nativewindow.width = screen_width;
	nativewindow.height = screen_height;
	vc_dispmanx_update_submit_sync(dispman_update);

	if ((surface = eglCreateWindowSurface(display, config, &nativewindow, NULL )) == EGL_NO_SURFACE) {
		LOG_PRINT_ERROR("Could not create EGL surface");
		return NULL;
	}

	/* connect the context to the surface */
	if (eglMakeCurrent(display, surface, surface, ctx) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not make GL context current!");
		return NULL;
	}

	/* clear the screen */
	int i;
	for (i = 0; i < 3; i++) {
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glFlush();
		glFinish();
		swap_buffers();
	}

	DEBUG_VPRINT(LOG_MODULE, "EGL window created (w=%d,h=%d)",
		*w, *h);

	/* initialize the GL driver */
	if ((surf = avbox_video_glinit(
		driver, screen_width, screen_height,
		swap_buffers)) == NULL) {
		LOG_PRINT_ERROR("GL setup failed");
		goto end;
	} else {
		DEBUG_PRINT(LOG_MODULE, "GL Driver Initialized");
	}
end:
	return surf;
}


static void
shutdown(void)
{
}


INTERNAL void
avbox_video_vc4_initft(struct mbv_drv_funcs * const funcs)
{
	memset(funcs, 0, sizeof(struct mbv_drv_funcs));
	funcs->init = &init;
	funcs->shutdown = &shutdown;
}
