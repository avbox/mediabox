#ifndef __AVBOX_VIDEO_X11__
#define __AVBOX_VIDEO_X11__

#include "video-drv.h"
#include <GLES2/gl2.h>


/**
 * Assigns an MMAL buffer to a GL texture.
 */
int
avbox_video_vc4_mmal2texture(void *buf, GLuint texture);


void
avbox_video_vc4_initft(struct mbv_drv_funcs * const funcs);

#endif
