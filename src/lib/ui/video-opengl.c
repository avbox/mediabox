#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>

#define GL_GLEXT_PROTOTYPES

#include <GL/gl.h>
#include <GL/glext.h>

#define LOG_MODULE "video-opengl"

#include "../debug.h"
#include "../log.h"
#include "../thread.h"
#include "../string_util.h"
#include "video.h"
#include "video-drv.h"


struct mbv_surface;

struct mbv_surface
{
	struct mbv_surface *parent;
	struct mbv_surface *real;
	GLuint texture;
	GLuint framebuffer;
	unsigned int lockflags;
	int bufsz;
	int pitch;
	int realx;
	int realy;
	int x;
	int y;
	int w;
	int h;
	void *buf;
};


/* GL driver */
static GLuint root_framebuffer;
static struct mbv_surface *root_surface;
static void (*wait_for_vsync)(void);

#ifndef NDEBUG
static pthread_t gl_thread;
#endif


static int
surface_doublebuffered(const struct mbv_surface * const surface)
{
	(void) surface;
	return 0;
}


static struct mbv_surface *
surface_new(struct mbv_surface *parent,
	const int x, const int y, const int w, const int h)
{
	GLenum err;
	struct mbv_surface *inst;

	if ((inst = malloc(sizeof(struct mbv_surface))) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}

	inst->x = x;
	inst->y = y;
	inst->w = w;
	inst->h = h;
	inst->parent = parent;
	inst->framebuffer = 0;
	inst->lockflags = 0;

	if (parent != NULL) {
		inst->real = parent->real;
		inst->realx = parent->realx + inst->x;
		inst->realy = parent->realy + inst->y;
		inst->pitch = parent->pitch;
		inst->bufsz = 0;
		inst->buf = ((uint8_t*)parent->buf) + (inst->pitch * inst->y) + inst->x;
		inst->texture = inst->parent->texture;
	} else {
		inst->real = inst;
		inst->realx = 0;
		inst->realy = 0;
		inst->pitch = ((w * 4) + 15) & ~15;
		inst->bufsz = inst->pitch * h;

		/* allocate a back-buffer in system memory */
		if ((errno = posix_memalign(&inst->buf, 16, inst->bufsz)) == -1) {
			ASSERT(errno == ENOMEM);
			return NULL;
		}

		/* create the texture */
		glGenTextures(1, &inst->texture);
		if ((err = glGetError()) != GL_NO_ERROR) {
			LOG_PRINT_ERROR("glGenTextures returned error!");
		}
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, inst->w, inst->h,
			0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		if ((err = glGetError()) != GL_NO_ERROR) {
			LOG_VPRINT_ERROR("Could not clear surface!: 0x%x", err);
			free(inst->buf);
			free(inst);
			inst = NULL;
		}
	}

	return inst;
}


static GLuint
surface_framebuffer(struct mbv_surface * const inst)
{
	if (inst->framebuffer == 0 && inst == inst->real) {
		GLenum err;
		glGenFramebuffers(1, &inst->framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, inst->framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->texture, 0);
		if ((err = glGetError()) != GL_NO_ERROR) {
			LOG_VPRINT_ERROR("GL error: %d", err);
		}
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			LOG_PRINT_ERROR("Could not create surface framebuffer!");
			glDeleteFramebuffers(1, &inst->framebuffer);
			return -1;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	return inst->framebuffer;
}


static void *
surface_lock(struct mbv_surface * const inst,
	const unsigned int flags, int *pitch)
{
	ASSERT(inst != NULL);
	ASSERT(inst->lockflags == 0);
	ASSERT(flags != 0);
	ASSERT(pthread_self() == gl_thread);

	if (flags & MBV_LOCKFLAGS_READ) {
		GLuint err;
		glPixelStorei(GL_PACK_ROW_LENGTH, inst->pitch / 4);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, inst->real->buf);
		if ((err = glGetError()) != GL_NO_ERROR) {
			LOG_VPRINT_ERROR("Could not lock surface. glGetTexImage() failed: 0x%x",
				err);
			return NULL;
		}
	}

	*pitch = inst->pitch;
	inst->lockflags = flags;
	return inst->buf;
}


static int
surface_blitbuf(
	struct mbv_surface * const inst,
	void *buf, int pitch, unsigned int flags, int width, int height, const int x, const int y)
{
	GLuint err;
	glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
	glBindTexture(GL_TEXTURE_2D, inst->texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, inst->realx + x, inst->realy + y, width, height,
		GL_BGRA, GL_UNSIGNED_BYTE, buf);
	if ((err = glGetError()) != GL_NO_ERROR) {
		LOG_VPRINT_ERROR("Could not upload surface!: 0x%x", err);
		return -1;
	}

	return 0;
}


static int
surface_blit(
	struct mbv_surface * const dst,
	struct mbv_surface * const src,
	unsigned int flags, int x, int y)
{
	GLuint fb;

	/* configure the destination surface as the framebuffer */
	if ((fb = surface_framebuffer(dst)) == 0) {
		LOG_PRINT_ERROR("Could not change framebuffer. Blit failed");
		return -1;
	}

	/* set the projection matrix */
	glViewport(0, 0, dst->w, dst->h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, src->w, src->h, 0.0, 1.0, -1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	/* render the src surface */
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glBindTexture(GL_TEXTURE_2D, src->texture);
	glBegin(GL_QUADS);
		glTexCoord2f(0.0, 1.0); glVertex2f(0, 0);
		glTexCoord2f(1.0, 1.0); glVertex2f(src->w, 0);
		glTexCoord2f(1.0, 0.0); glVertex2f(src->w, src->h);
		glTexCoord2f(0.0, 0.0); glVertex2f(0, src->h);
	glEnd();
	glFlush();

	/* restore state */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, root_surface->w, root_surface->h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, root_surface->w, root_surface->h, 0.0, 1.0, -1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	return 0;
}


static int
surface_scaleblit(
	struct mbv_surface * const dst,
	struct mbv_surface * const src,
	unsigned int flags,
	const int x, const int y, const int w, const int h)
{
	GLuint fb;

	/* configure the destination surface as the framebuffer */
	if ((fb = surface_framebuffer(dst)) == 0) {
		LOG_PRINT_ERROR("Could not change framebuffer. Blit failed");
		return -1;
	}

	/* set the projection matrix */
	glViewport(0, 0, dst->w, dst->h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, dst->w, dst->h, 0.0, 1.0, -1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	/* render the src surface */
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glBindTexture(GL_TEXTURE_2D, src->texture);
	glBegin(GL_QUADS);
		glTexCoord2f(0.0, 1.0); glVertex2f(x, y);
		glTexCoord2f(1.0, 1.0); glVertex2f(x + w, y);
		glTexCoord2f(1.0, 0.0); glVertex2f(x + w, y + h);
		glTexCoord2f(0.0, 0.0); glVertex2f(x, y + h);
	glEnd();
	glFlush();

	/* restore state */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, root_surface->w, root_surface->h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, root_surface->w, root_surface->h, 0.0, 1.0, -1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	return 0;
}


static void
surface_unlock(struct mbv_surface * const inst)
{
	ASSERT(inst != NULL);
	ASSERT(inst->lockflags != 0);

	if (inst->lockflags & MBV_LOCKFLAGS_WRITE) {
		GLuint err;
		glPixelStorei(GL_UNPACK_ROW_LENGTH, inst->pitch / 4);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, inst->realx, inst->realy, inst->w, inst->h,
			GL_BGRA, GL_UNSIGNED_BYTE, inst->buf);
		if ((err = glGetError()) != GL_NO_ERROR) {
			LOG_VPRINT_ERROR("Could not upload surface!: 0x%x", err);
		}
	}
	inst->lockflags = 0;
}


static void
surface_update(struct mbv_surface * const inst, int blitflags, int update)
{
	if (inst->parent != NULL) {
		return;
	}

	if (blitflags & MBV_BLITFLAGS_ALPHABLEND) {
		glEnable(GL_BLEND);
	}

	if (update || inst == root_surface) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glBegin(GL_QUADS);
			glTexCoord2f(0.0, 0.0); glVertex2f(inst->x, inst->y);
			glTexCoord2f(1.0, 0.0); glVertex2f(inst->x + inst->w, inst->y);
			glTexCoord2f(1.0, 1.0); glVertex2f(inst->x + inst->w, inst->y + inst->h);
			glTexCoord2f(0.0, 1.0); glVertex2f(inst->x, inst->y + inst->h);
		glEnd();

		/* if this is a fullscreen repaint wait for vsync */
		if (inst == root_surface) {
			wait_for_vsync();
		}
	} else {
		glBindFramebuffer(GL_FRAMEBUFFER, root_framebuffer);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glBegin(GL_QUADS);
			glTexCoord2f(0.0, 1.0); glVertex2f(inst->x, root_surface->h - (inst->y + inst->h));
			glTexCoord2f(1.0, 1.0); glVertex2f(inst->x + inst->w, root_surface->h - (inst->y + inst->h));
			glTexCoord2f(1.0, 0.0); glVertex2f(inst->x + inst->w, root_surface->h - inst->y);
			glTexCoord2f(0.0, 0.0); glVertex2f(inst->x, root_surface->h - inst->y);
		glEnd();
	}

	glFlush();

	if (blitflags & MBV_BLITFLAGS_ALPHABLEND) {
		glDisable(GL_BLEND);
	}
}


static void
surface_destroy(struct mbv_surface * const inst)
{
	ASSERT(inst != NULL);
	if (inst->framebuffer != 0) {
		glDeleteFramebuffers(1, &inst->framebuffer);
	}
	glDeleteTextures(1, &inst->texture);
	if (inst->bufsz != 0) {
		free(inst->buf);
	}
	free(inst);
}


static void
init_func_table(struct mbv_drv_funcs * const funcs)
{
	funcs->surface_new = &surface_new;
	funcs->surface_lock = &surface_lock;
	funcs->surface_unlock = &surface_unlock;
	funcs->surface_blitbuf = &surface_blitbuf;
	funcs->surface_blit = &surface_blit;
	funcs->surface_scaleblit = &surface_scaleblit;
	funcs->surface_update = &surface_update;
	funcs->surface_doublebuffered = &surface_doublebuffered;
	funcs->surface_destroy = &surface_destroy;
}


/**
 * Initialize the opengl driver
 */
struct mbv_surface *
avbox_video_glinit(struct mbv_drv_funcs * const funcs, int width, const int height,
	void (*wait_for_vsync_fn)(void))
{
	GLenum err;

#ifndef NDEBUG
	/* remember the main thread */
	gl_thread = pthread_self();
#endif

	/* re-initialize the driver function table with GL
	 * functions */
	init_func_table(funcs);
	wait_for_vsync = wait_for_vsync_fn;

	/* set the view port and clear screen and this is it.
	 * for now on we use opengl */
	glEnable(GL_TEXTURE_2D);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, width, height, 0.0, 1.0, -1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* check for gl errors so far */
	if ((err = glGetError()) != GL_NO_ERROR) {
		LOG_PRINT_ERROR("GL setup failed");
		return NULL;
	} else {
		DEBUG_PRINT(LOG_MODULE, "GL Context Initialized");
	}

	/* create the root surface */
	if ((root_surface = surface_new(NULL, 0, 0, width, height)) == NULL) {
		LOG_PRINT_ERROR("Could not create root surface");
	}

	/* create a framebuffer for rendering to the root surface */
	if ((root_framebuffer = surface_framebuffer(root_surface)) == 0) {
		LOG_PRINT_ERROR("Could not change fbo. Blit failed");
		surface_destroy(root_surface);
		root_surface = NULL;
	}

	LOG_PRINT_INFO("OpenGL Compositor Initialized");
	LOG_VPRINT_INFO("Vendor:\t%s", glGetString(GL_VENDOR));
	LOG_VPRINT_INFO("Renderer:\t%s", glGetString(GL_RENDERER));
	LOG_VPRINT_INFO("Version:\t%s", glGetString(GL_VERSION));
	LOG_VPRINT_INFO("GLSL:\t%s", glGetString(GL_SHADING_LANGUAGE_VERSION));

#if 0
	char *gl_exts;
	LOG_PRINT_INFO("Extensions:");

	if ((gl_exts = strdup((const char *)glGetString(GL_EXTENSIONS))) != NULL) {
		char *next, *current = gl_exts;
		while (current != NULL) {
			if ((next = strstr(current, " ")) != NULL) {
				*next++ = '\0';
			}
			LOG_VPRINT_INFO("\t%s", current);
			current = next;
		}
		free(gl_exts);
	}
#endif

	return root_surface;
}
