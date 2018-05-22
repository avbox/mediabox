#ifdef HAVE_CONFIG_H
#       include <libavbox/config.h>
#endif
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>

#define GL_GLEXT_PROTOTYPES

#ifdef ENABLE_GLES2
#	include <GLES2/gl2.h>
#else
#	include <GL/gl.h>
#endif

#ifdef ENABLE_VC4
#	include "video-vc4.h"
#	include <GLES2/gl2ext.h>
#endif

#define LOG_MODULE "video-opengl"

#include <libavbox/avbox.h>


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
static GLuint vertex_buffer;
static GLint bgra_texcoords, bgra_pos, bgra_texture, bgra_target;
static GLint yuv420p_y, yuv420p_u, yuv420p_v, yuv420p_pos, yuv420p_texcoords;
static struct mbv_surface *root_surface;
static GLfloat texcoords[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
static GLfloat texcoords_yuv[] = {
	0.0f, 1.0f,
	1.0f, 1.0f,
	0.0f, 0.0f,
	1.0f, 0.0f,
};
static void (*swap_buffers)(void);

#ifdef ENABLE_VC4
static GLuint mmal_program;
static GLint mmal_texcoords, mmal_pos, mmal_texture;
#endif


#define TARGET_SURFACE	(0)
#define TARGET_DISPLAY	(1)


#ifndef NDEBUG
static pthread_t gl_thread;

#define DEBUG_ERROR_CHECK() \
do { \
	GLuint _err; \
	if ((_err = glGetError()) != GL_NO_ERROR) { \
		switch (_err) { \
		case GL_INVALID_OPERATION: LOG_VPRINT_ERROR("GL error (%d): Invalid operation", __LINE__); break; \
		case GL_OUT_OF_MEMORY: LOG_VPRINT_ERROR("GL error (%d): Out of memory", __LINE__); break; \
		case GL_INVALID_VALUE: LOG_VPRINT_ERROR("GL error (%d): Invalid value", __LINE__); break; \
		default: LOG_VPRINT_ERROR("GL error (%d): 0x%x", __LINE__, _err); break; \
		} \
		abort(); \
	} \
} while (0)

#define DEBUG_THREAD_CHECK()	ASSERT(pthread_self() == gl_thread)
#else
#define DEBUG_ERROR_CHECK()	(void) 0
#define DEBUG_THREAD_CHECK()	(void) 0
#endif


static inline void
avbox_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
	GLenum format, GLenum type, const GLvoid *data, int pitch, int pixsz)
{
#if 1 || defined(ENABLE_GLES2)
	/* this is way too slow. find a better way. */
	int yy;
	const uint8_t *pdata = ((const uint8_t*)data) + ((h - 1) * pitch);
	(void) pixsz;
	for (yy = y; yy < y + h; pdata -= pitch, yy++) {
		glTexSubImage2D(target, level, x, yy, w, 1, format, type, pdata);
	}
#else
	glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / pixsz);
	glTexSubImage2D(target, level, x, y, w, h, format, type, data);
#endif
}


static inline void
avbox_glTexImage2D(GLenum target, GLint level,
	GLint internal_format, GLsizei w, GLsizei h,
	GLint border, GLenum format, GLenum type, const GLvoid * data, int pitch, int pixsz)
{
	glTexImage2D(target, level, internal_format, w, h,
		border, format, type, NULL);
	avbox_glTexSubImage2D(target, level, 0, 0, w, h, format,
		type, data, pitch, pixsz);
}


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
	inst->buf = NULL;

	if (parent != NULL) {
		inst->real = parent->real;
		inst->realx = parent->realx + inst->x;
		inst->realy = parent->realy + inst->y;
		inst->texture = parent->texture;
		inst->pitch = parent->pitch;
		inst->bufsz = 0;
		inst->buf = ((uint8_t*)parent->buf) + (inst->pitch * inst->y) + (inst->x * 4);

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
#ifndef NDEBUG
		GLenum status;
		if ((status = glCheckFramebufferStatus(GL_FRAMEBUFFER)) != GL_FRAMEBUFFER_COMPLETE) {
			const char *str;
			switch (status) {
			case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: str = "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT"; break;
			case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: str = "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT"; break;
			/* case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS: str = "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS"; break; */
			case GL_FRAMEBUFFER_UNSUPPORTED: str = "GL_FRAMEBUFFER_UNSUPPORTED"; break;
			default: str = "???";
			}
			LOG_VPRINT_ERROR("Could not create surface framebuffer (status=0x%x): %s",
				status, str);
			glDeleteFramebuffers(1, &inst->framebuffer);
			abort();
			return -1;
		}
#endif
		ASSERT(inst->framebuffer != 0);
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
	ASSERT(inst->buf != NULL);
	ASSERT(flags != 0);

	*pitch = inst->pitch;

	if (flags & MBV_LOCKFLAGS_READ) {
		#if 0
		glPixelStorei(GL_PACK_ROW_LENGTH, inst->pitch / 4);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, inst->real->buf);
		DEBUG_ERROR_CHECK();
		#endif
	}

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

		glVertexAttribPointer(yuv420p_texcoords, 2, GL_FLOAT, GL_FALSE, 0, texcoords_yuv);
		glEnableVertexAttribArray(yuv420p_texcoords);

		/* upload each plane to a texture */
		glGenTextures(3, planes);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
		glBindTexture(GL_TEXTURE_2D, planes[0]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		if (pitch[0] == w) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0,
				GL_ALPHA, GL_UNSIGNED_BYTE, buf[0]);
		} else {
			int i;
			uint8_t *pbuf = buf[0];
			glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0,
				GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
			for (i = 0; i < h; i++) {
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i, w, 1,
					GL_ALPHA, GL_UNSIGNED_BYTE, pbuf);
				pbuf += pitch[0];
			}
		}
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glBindTexture(GL_TEXTURE_2D, planes[1]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		if (pitch[1] == uv_w) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, uv_w, uv_h, 0,
				GL_ALPHA, GL_UNSIGNED_BYTE, buf[1]);
		} else {
			int i;
			uint8_t *pbuf = buf[1];
			glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, uv_w, uv_h, 0,
				GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
			for (i = 0; i < uv_h; i++) {
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i, uv_w, 1,
					GL_ALPHA, GL_UNSIGNED_BYTE, pbuf);
				pbuf += pitch[1];
			}
		}

		DEBUG_ERROR_CHECK();

		glBindTexture(GL_TEXTURE_2D, planes[2]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		if (pitch[2] == uv_w) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, uv_w, uv_h, 0,
				GL_ALPHA, GL_UNSIGNED_BYTE, buf[2]);
		} else {
			int i;
			uint8_t *pbuf = buf[2];
			glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, uv_w, uv_h, 0,
				GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
			for (i = 0; i < uv_h; i++) {
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i, uv_w, 1,
					GL_ALPHA, GL_UNSIGNED_BYTE, pbuf);
				pbuf += pitch[2];
			}
		}

		DEBUG_ERROR_CHECK();

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

		glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
		glVertexAttribPointer(yuv420p_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
		glEnableVertexAttribArray(yuv420p_pos);

		/* convert and render to texture */
		glBindFramebuffer(GL_FRAMEBUFFER, surface_framebuffer(inst));
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, inst->texture, 0);
		glViewport(inst->x, inst->h - (y + h), inst->w, inst->h);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDeleteTextures(3, planes);
		DEBUG_ERROR_CHECK();
		return 0;
	}
#ifdef ENABLE_VC4
	case AVBOX_PIXFMT_MMAL:
	{
		GLuint texture;

		/* assign mmal buffer to texture */
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
		glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		avbox_video_vc4_mmal2texture(buf[0], texture);
		DEBUG_ERROR_CHECK();

		glBindFramebuffer(GL_FRAMEBUFFER, surface_framebuffer(inst));
		DEBUG_ERROR_CHECK();

		glVertexAttribPointer(mmal_texcoords, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
		glEnableVertexAttribArray(mmal_texcoords);

		glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
		glVertexAttribPointer(mmal_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
		glEnableVertexAttribArray(mmal_pos);
		DEBUG_ERROR_CHECK();

		glUseProgram(mmal_program);
		DEBUG_ERROR_CHECK();
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
		DEBUG_ERROR_CHECK();

		glViewport(x, inst->h - (y + h), w, h);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDeleteTextures(1, &texture);
		DEBUG_ERROR_CHECK();
		break;
	}
#endif
	case AVBOX_PIXFMT_BGRA:
	{
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		avbox_glTexSubImage2D(GL_TEXTURE_2D, 0, inst->realx + x, inst->realy + y, w, h,
			GL_RGBA, GL_UNSIGNED_BYTE, buf[0], pitch[0], 4);
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

	glBindFramebuffer(GL_FRAMEBUFFER, surface_framebuffer(dst));
	DEBUG_ERROR_CHECK();

	glVertexAttribPointer(bgra_texcoords, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
	glEnableVertexAttribArray(bgra_texcoords);

	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glVertexAttribPointer(bgra_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(bgra_pos);

	glUseProgram(bgra_program);
	glBindTexture(GL_TEXTURE_2D, src->texture);
	glUniform1i(bgra_texture, 0);
	glUniform1i(bgra_target, TARGET_SURFACE);

	glViewport(x, dst->h - (y + h), w, h);
	DEBUG_ERROR_CHECK();
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	DEBUG_ERROR_CHECK();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	if (flags & MBV_BLITFLAGS_ALPHABLEND) {
		glDisable(GL_BLEND);
	}

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
		const int y = inst->real->h - (inst->realy + inst->h);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glBindTexture(GL_TEXTURE_2D, inst->texture);
		avbox_glTexSubImage2D(GL_TEXTURE_2D, 0, inst->realx, y, inst->w, inst->h,
			GL_RGBA, GL_UNSIGNED_BYTE, inst->buf, inst->pitch, 4);
		DEBUG_ERROR_CHECK();
	}
	inst->lockflags = 0;
}


static inline
void
surface_render(struct mbv_surface * const inst, unsigned int flags, GLenum buffer)
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	DEBUG_ERROR_CHECK();

	glVertexAttribPointer(bgra_texcoords, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
	glEnableVertexAttribArray(bgra_texcoords);

	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glUseProgram(bgra_program);
	glBindTexture(GL_TEXTURE_2D, inst->texture);
	glUniform1i(bgra_texture, 0);
	glUniform1i(bgra_target, TARGET_DISPLAY);
	glViewport(inst->x, inst->y, inst->w, inst->h);
	DEBUG_ERROR_CHECK();

#ifndef ENABLE_GLES2
	/* glDrawBuffer(buffer); */
#endif
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	DEBUG_ERROR_CHECK();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
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
			/* this works with OpenGL under X but it doesn't
			 * work under DRM/EGL so at the moment this code will
			 * never run as it's disable on video.c */
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
	DEBUG_ERROR_CHECK();

#ifndef NDEBUG
	GLsizei len;
	char log[4096];
	/* print logs */
	glGetShaderInfoLog(vertex, 4096, &len, log);
	if (strcmp(log, "") && len > 0) {
		LOG_VPRINT_ERROR("vertex: %s", log);
	}
	glGetShaderInfoLog(fragment, 4096, &len, log);
	if (strcmp(log, "") && len > 0) {
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
	if (strcmp(log, "") && len > 0) {
		LOG_VPRINT_ERROR("Link: %s", log);
	}
#endif

	DEBUG_ERROR_CHECK();

	return program;

}


static void
avbox_video_opengl_prepare_shaders(void)
{
	const char * vertex_source = GLSL(120,
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
		uniform int target;
		varying vec2 v_texcoords;
		void main()
		{
			if (target == 1) {
				gl_FragColor = vec4(texture2D(texture, v_texcoords).rgb, 1.0);
			} else {
				gl_FragColor = texture2D(texture, v_texcoords).bgra;
			}
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

			gl_FragColor = vec4(b,g,r,1);

		});


	DEBUG_PRINT(LOG_MODULE, "Compiling shaders...");
	bgra_program = avbox_video_opengl_compile_program("bgra",
		vertex_source, bgra_fragment_source);
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
	bgra_target = glGetUniformLocation(bgra_program, "target");
	DEBUG_ERROR_CHECK();

#ifdef ENABLE_VC4
	const char * mmal_fragment_source =
		"#extension GL_OES_EGL_image_external : require\n"
		"varying vec2 v_texcoords;\n"
		"uniform samplerExternalOES zztexture;\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = texture2D(zztexture, v_texcoords).bgra;\n"
		"}\n";

	mmal_program = avbox_video_opengl_compile_program("mmal",
		vertex_source, mmal_fragment_source);
	mmal_pos = glGetAttribLocation(mmal_program, "pos");
	mmal_texcoords = glGetAttribLocation(mmal_program, "texcoords");
	mmal_texture = glGetUniformLocation(mmal_program, "zztexture");
	DEBUG_ERROR_CHECK();
#endif


}


/**
 * Initialize the opengl driver
 */
struct mbv_surface *
avbox_video_glinit(struct mbv_drv_funcs * const funcs, int width, const int height,
	void (*swap_buffers_fn)(void))
{

	const GLfloat vertices[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };

	DEBUG_VPRINT(LOG_MODULE, "Initializing GL driver (width=%d, height=%d)",
		width, height);

#ifndef NDEBUG
	/* remember the main thread */
	gl_thread = pthread_self();
#endif

	/* re-initialize the driver function table with GL
	 * functions */
	init_func_table(funcs);
	swap_buffers = swap_buffers_fn;

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	DEBUG_ERROR_CHECK();

	/* create the root surface */
	if ((root_surface = surface_new(NULL, 0, 0, width, height)) == NULL) {
		LOG_PRINT_ERROR("Could not create root surface");
	}

	LOG_PRINT_INFO("OpenGL Compositor Initialized");
	LOG_VPRINT_INFO("Vendor:\t%s", glGetString(GL_VENDOR));
	LOG_VPRINT_INFO("Renderer:\t%s", glGetString(GL_RENDERER));
	LOG_VPRINT_INFO("Version:\t%s", glGetString(GL_VERSION));
#ifndef ENABLE_GLES2
	LOG_VPRINT_INFO("GLSL:\t%s", glGetString(GL_SHADING_LANGUAGE_VERSION));
#endif

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

	/* upload surface vertices to buffer object */
	glGenBuffers(1, &vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), 
		vertices, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glVertexAttribPointer(bgra_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(bgra_pos);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	DEBUG_ERROR_CHECK();

	glVertexAttribPointer(yuv420p_texcoords, 2, GL_FLOAT, GL_FALSE, 0, texcoords_yuv);
	glEnableVertexAttribArray(yuv420p_texcoords);

	return root_surface;
}
