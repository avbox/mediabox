#ifndef __MB_VIDEO_DIRECTFB__
#define __MB_VIDEO_DIRECTFB__

/**
 * mb_video_clear()
 */
void
mbv_clear(void);

/**
 * mbv_init() -- Initialize video device
 */
void
mbv_init(int argc, char **argv);

void
mbv_destroy();

#endif

