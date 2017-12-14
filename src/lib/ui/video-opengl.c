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


/* why does adding the version directive causes the shader to
 * fail on some systems? Do we need windows line breaks on shaders ? */
#define GLSL(version, shader) #shader


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
static GLuint bgra_program = 0, yuv420p_program = 0;
static GLint bgra_texcoords, bgra_pos, bgra_texture;
static GLint yuv420p_y, yuv420p_u, yuv420p_v, yuv420p_pos, yuv420p_texcoords;
static struct mbv_surface *root_surface;
static const GLfloat surface_texcoords[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
static const GLfloat display_texcoords[] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };
static const GLfloat vertices[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
static const GLenum draw_buffers[] = { GL_COLOR_ATTACHMENT0, GL_NONE };
static void (*swap_buffers)(void);


#ifndef NDEBUG
static pthread_t gl_thread;

#define DEBUG_ERROR_CHECK() \
do { \
	GLuint _err; \
	if ((_err = glGetError()) != GL_NO_ERROR) { \
		switch (_err) { \
		case GL_INVALID_OPERATION: LOG_VPRINT_ERROR("GL error (%d): Invalid operation", __LINE__); break; \
		default: LOG_VPRINT_ERROR("GL error: 0x%x", _err); abort(); \
		} \
	} \
} while (0)

#define DEBUG_THREAD_CHECK()	ASSERT(pthread_self() == gl_thread)
#else
#define DEBUG_ERROR_CHECK()	(void) 0
#define DEBUG_THREAD_CHECK()	(void) 0
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
	struct mbv_surface *inst;

	DEBUG_THREAD_CHECK();

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
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, inst->w, inst->h,
			0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		DEBUG_ERROR_CHECK();
	}

	return inst;
}


static GLuint
surface_framebuffer(struct mbv_surface * const inst)
{
	if (inst->framebuffer == 0 && inst == inst->real) {
		glGenFramebuffers(1, &inst->framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, inst->framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->texture, 0);
#ifdef NDEBUG
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			LOG_PRINT_ERROR("Could not create surface framebuffer!");
			glDeleteFramebuffers(1, &inst->framebuffer);
			return -1;
		}
#endif
	}
	return inst->framebuffer;
}


static void *
surface_lock(struct mbv_surface * const inst,
	const unsigned int flags, int *pitch)
{
	DEBUG_THREAD_CHECK();
	ASSERT(inst != NULL);
	ASSERT(inst->lockflags == 0);
	ASSERT(flags != 0);

	if (flags & MBV_LOCKFLAGS_READ) {
		glPixelStorei(GL_PACK_ROW_LENGTH, inst->pitch / 4);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, inst->real->buf);
		DEBUG_ERROR_CHECK();
	}

	*pitch = inst->pitch;
	inst->lockflags = flags;
	return inst->buf;
}


static int
surface_blitbuf(
	struct mbv_surface * const inst,
	unsigned int pix_fmt, void **buf, int *pitch, unsigned int flags,
	int w, int h, const int x, const int y)
{
	DEBUG_THREAD_CHECK();

	switch (pix_fmt) {
	case AVBOX_PIXFMT_YUV420P:
	{
		GLuint planes[3];
		const int uv_w = w >> 1, uv_h = h >> 1;

		/* upload each plane to a texture */
		glGenTextures(3, planes);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch[0]);
		glBindTexture(GL_TEXTURE_2D, planes[0]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0,
			GL_ALPHA, GL_UNSIGNED_BYTE, buf[0]);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch[1]);
		glBindTexture(GL_TEXTURE_2D, planes[1]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, uv_w, uv_h, 0,
			GL_ALPHA, GL_UNSIGNED_BYTE, buf[1]);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch[2]);
		glBindTexture(GL_TEXTURE_2D, planes[2]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, uv_w, uv_h, 0,
			GL_ALPHA, GL_UNSIGNED_BYTE, buf[2]);

		/* prepare shaders */
		glUseProgram(yuv420p_program);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, planes[0]);
		glUniform1i(yuv420p_y, 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, planes[1]);
		glUniform1i(yuv420p_u, 1);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, planes[2]);
		glUniform1i(yuv420p_v, 2);
		glActiveTexture(GL_TEXTURE0);
		glVertexAttribPointer(yuv420p_pos, 2, GL_FLOAT, GL_FALSE, 0, vertices);
		glVertexAttribPointer(yuv420p_texcoords, 2, GL_FLOAT, GL_FALSE, 0, surface_texcoords);
		glEnableVertexAttribArray(yuv420p_pos);
		glEnableVertexAttribArray(yuv420p_texcoords);

		/* convert and render to texture */
		glBindFramebuffer(GL_FRAMEBUFFER, surface_framebuffer(inst));
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, inst->texture, 0);
		glViewport(inst->x, inst->y, inst->w, inst->h);
		glDrawBuffers(1, draw_buffers);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDeleteTextures(3, planes);
		DEBUG_ERROR_CHECK();
		return 0;

	}
	case AVBOX_PIXFMT_BGRA:
	{
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, *pitch / 4);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, inst->realx + x, inst->realy + y, w, h,
			GL_BGRA, GL_UNSIGNED_BYTE, *buf);
		DEBUG_ERROR_CHECK();
		break;
	}
	default:
		abort();
	}

	return 0;
}


static inline int
surface_scaleblit(
	struct mbv_surface * const dst,
	struct mbv_surface * const src,
	unsigned int flags,
	const int x, const int y, const int w, const int h)
{
	DEBUG_THREAD_CHECK();

	if (flags & MBV_BLITFLAGS_ALPHABLEND) {
		glEnable(GL_BLEND);
	}
	glUseProgram(bgra_program);
	glBindTexture(GL_TEXTURE_2D, src->texture);
	glUniform1i(bgra_texture, 0);
	glVertexAttribPointer(bgra_pos, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(bgra_texcoords, 2, GL_FLOAT, GL_FALSE, 0, surface_texcoords);
	glEnableVertexAttribArray(bgra_pos);
	glEnableVertexAttribArray(bgra_texcoords);
	glBindFramebuffer(GL_FRAMEBUFFER, surface_framebuffer(dst));
	glDrawBuffers(1, draw_buffers);
	glViewport(x, y, w, h);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (flags & MBV_BLITFLAGS_ALPHABLEND) {
		glDisable(GL_BLEND);
	}

	DEBUG_ERROR_CHECK();
	return 0;
}


static inline int
surface_blit(
	struct mbv_surface * const dst,
	struct mbv_surface * const src,
	unsigned int flags, int x, int y)
{
	return surface_scaleblit(dst, src, flags, x, y, src->w, src->h);
}



static void
surface_unlock(struct mbv_surface * const inst)
{
	DEBUG_THREAD_CHECK();
	ASSERT(inst != NULL);
	ASSERT(inst->lockflags != 0);

	if (inst->lockflags & MBV_LOCKFLAGS_WRITE) {
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, inst->pitch / 4);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, inst->realx, inst->realy, inst->w, inst->h,
			GL_BGRA, GL_UNSIGNED_BYTE, inst->buf);
		DEBUG_ERROR_CHECK();
	}
	inst->lockflags = 0;
}


static inline
void
surface_render(struct mbv_surface * const inst, unsigned int flags, GLenum buffer)
{
	if (flags & MBV_BLITFLAGS_ALPHABLEND) {
		glEnable(GL_BLEND);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glUseProgram(bgra_program);
	glBindTexture(GL_TEXTURE_2D, inst->texture);
	glUniform1i(bgra_texture, 0);
	glVertexAttribPointer(bgra_pos, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(bgra_texcoords, 2, GL_FLOAT, GL_FALSE, 0, display_texcoords);
	glEnableVertexAttribArray(bgra_pos);
	glEnableVertexAttribArray(bgra_texcoords);
	glViewport(inst->x, root_surface->h - (inst->y + inst->h), inst->w, inst->h);
	glDrawBuffer(buffer);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFlush();
	if (flags & MBV_BLITFLAGS_ALPHABLEND) {
		glDisable(GL_BLEND);
	}
	DEBUG_ERROR_CHECK();
}


static void
surface_update(struct mbv_surface * const inst, int blitflags, int update)
{
	DEBUG_THREAD_CHECK();

	if (inst->parent != NULL) {
		return;
	}

	if (inst == root_surface) {
		surface_render(inst, 0, GL_BACK);
		swap_buffers();
	} else {
		if (update) {
			surface_render(inst, blitflags, GL_FRONT);
		} else {
			surface_blit(root_surface, inst, blitflags, inst->x, inst->y);
		}
	}
}


static void
surface_destroy(struct mbv_surface * const inst)
{
	DEBUG_THREAD_CHECK();
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


static GLint
avbox_video_opengl_compile_program(const char *name,
	const char *vertex_src, const char *fragment_src)
{
	GLuint vertex, fragment, program;

	DEBUG_VPRINT(LOG_MODULE, "Compiling program \"%s\"...",
		name);

	/* "read" the source */
	vertex = glCreateShader(GL_VERTEX_SHADER);
	fragment = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(vertex, 1, &vertex_src, NULL);
	glShaderSource(fragment, 1, &fragment_src, NULL);

	/* compile the program */
	program = glCreateProgram();
	glCompileShader(vertex);
	glCompileShader(fragment);

#ifndef NDEBUG
	GLsizei len;
	char log[4096];
	/* print logs */
	glGetShaderInfoLog(vertex, 4096, &len, log);
	if (strcmp(log, "")) {
		LOG_VPRINT_ERROR("vertex: %s", log);
	}
	glGetShaderInfoLog(fragment, 4096, &len, log);
	if (strcmp(log, "")) {
		LOG_VPRINT_ERROR("fragment: %s", log);
	}
#endif
	/* link the shaders */
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	DEBUG_ERROR_CHECK();

#ifndef NDEBUG
	glGetProgramInfoLog(program, 4086, &len, log);
	if (strcmp(log, "")) {
		LOG_VPRINT_ERROR("Link: %s", log);
	}
#endif

	DEBUG_ERROR_CHECK();

	return program;

}


static void
avbox_video_opengl_prepare_shaders(void)
{
	const char * bgra_vertex_source = GLSL(120,
		attribute vec4 pos;
		attribute vec2 texcoords;
		varying vec2 v_texcoords;
		void main()
		{
			v_texcoords  = texcoords.xy;
			gl_Position = pos;
		});
	const char * bgra_fragment_source = GLSL(120,
		uniform sampler2D texture;
		varying vec2 v_texcoords;
		void main()
		{
			gl_FragColor = texture2D(texture, v_texcoords).rgba;
		});

	const char * yuv420p_vertex_source = GLSL(120,
		attribute vec4 pos;
		attribute vec2 texcoords;
		varying vec2 v_texcoords;
		void main()
		{
			v_texcoords = texcoords.xy;
			gl_Position = pos;
		});
	const char * yuv420p_fragment_source = GLSL(130,
		varying vec2 v_texcoords;
		uniform sampler2D plane_y;
		uniform sampler2D plane_u;
		uniform sampler2D plane_v;
		void main() {
			/* https://en.wikipedia.org/wiki/YUV#Y%E2%80%B2UV444_to_RGB888_conversion
			 * https://www.fourcc.org/fccyvrgb.php#mikes_answer */

			/* this is the alg given by Kolyvan at
			 * https://stackoverflow.com/questions/12428108/ios-how-to-draw-a-yuv-image-using-opengl
			 * but with the Y offset by -0.0627 (16 integer) as suggested on some other site.
			 * It looks awesome */
			float y = texture2D(plane_y, v_texcoords).a - 0.0627;
			float u = texture2D(plane_u, v_texcoords).a - 0.5;
			float v = texture2D(plane_v, v_texcoords).a - 0.5;
			float r = y + (v * 1.402);
			float g = y - (u * 0.344) - (v * 0.714);
			float b = y + (u * 1.772);

			/* this is the first formula given on the above wikipedia link
			 * minus the 0.5 offset (+128 on the page) -- doesn't look quite right */
			/*
			float y = texture2D(plane_y, v_texcoords).a * 1.1686 - 0.0627;
			float u = texture2D(plane_u, v_texcoords).a - 0.5;
			float v = texture2D(plane_v, v_texcoords).a - 0.5;
			float r = y + 1.6039 * v;
			float g = y + 0.3921 * u + 0.8156 * v;
			float b = y + 2.0235 * u;
			*/

			/* this is the ITU-R formula on the wikipedia page with
			 * Y offset by -0.0627 */
			/*
			float y = texture2D(plane_y, v_texcoords).a - 0.0627;
			float u = texture2D(plane_u, v_texcoords).a - 0.5;
			float v = texture2D(plane_v, v_texcoords).a - 0.5;
			float r = y + 1.1402 * v;
			float g = y + 0.344 * u + 0.714 * v;
			float b = y + 1.1772 * u;
			*/

			gl_FragColor = vec4(r,g,b,1);

		});


	DEBUG_PRINT(LOG_MODULE, "Compiling shaders...");
	bgra_program = avbox_video_opengl_compile_program("bgra",
		bgra_vertex_source, bgra_fragment_source);
	yuv420p_program = avbox_video_opengl_compile_program("yuv420p",
		yuv420p_vertex_source, yuv420p_fragment_source);

	/* get uniform and attribute locations */
	yuv420p_y = glGetUniformLocation(yuv420p_program, "plane_y");
	yuv420p_u = glGetUniformLocation(yuv420p_program, "plane_u");
	yuv420p_v = glGetUniformLocation(yuv420p_program, "plane_v");
	yuv420p_pos = glGetAttribLocation(yuv420p_program, "pos");
	yuv420p_texcoords = glGetAttribLocation(yuv420p_program, "texcoords");
	DEBUG_ERROR_CHECK();

	bgra_pos = glGetAttribLocation(bgra_program, "pos");
	bgra_texcoords = glGetAttribLocation(bgra_program, "texcoords");
	bgra_texture = glGetUniformLocation(bgra_program, "texture");
	DEBUG_ERROR_CHECK();
}


/**
 * Initialize the opengl driver
 */
struct mbv_surface *
avbox_video_glinit(struct mbv_drv_funcs * const funcs, int width, const int height,
	void (*swap_buffers_fn)(void))
{
#ifndef NDEBUG
	/* remember the main thread */
	gl_thread = pthread_self();
#endif

	/* re-initialize the driver function table with GL
	 * functions */
	init_func_table(funcs);
	swap_buffers = swap_buffers_fn;

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	/* create the root surface */
	if ((root_surface = surface_new(NULL, 0, 0, width, height)) == NULL) {
		LOG_PRINT_ERROR("Could not create root surface");
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

	avbox_video_opengl_prepare_shaders();

	return root_surface;
}
