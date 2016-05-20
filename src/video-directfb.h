#ifndef __MB_VIDEO_DIRECTFB__
#define __MB_VIDEO_DIRECTFB__

/**
 * mb_video_clear()
 */
void
mbv_dfb_clear(void);

int
mbv_dfb_screen_width_get(void);

int
mbv_dfb_screen_height_get(void);

struct mbv_window*
mbv_dfb_window_new(
        char *title,
        int x,
        int y,
        int width,
        int height);

void
mbv_dfb_window_show(struct mbv_window *win);

void
mbv_dfb_window_hide(struct mbv_window *win);

void
mbv_dfb_window_destroy(struct mbv_window *win);


/**
 * mbv_init() -- Initialize video device
 */
void
mbv_dfb_init(int argc, char **argv);

void
mbv_dfb_destroy();

#endif
