#ifndef __AVBOX_VIDEO_SOFT
#define __AVBOX_VIDEO_SOFT


/**
 * Initialize the software renderer
 */
struct mbv_surface *
avbox_video_softinit(struct mbv_drv_funcs * const funcs,
	uint8_t *front_pixels, uint8_t *back_pixels, const int w, const int h, const int pitch,
	void (*wait_for_vsync_fn)(void), void (*swap_buffers_fn)(void));


#endif
