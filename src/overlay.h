#ifndef __MBOX_OVERLAY_H__
#define __MBOX_OVERLAY_H__

#define MBOX_OVERLAY_STATE_READY	(0)
#define MBOX_OVERLAY_STATE_PLAYING	(1)
#define MBOX_OVERLAY_STATE_PAUSED 	(2)


struct mbox_overlay;
struct avbox_player;

/**
 * Show the overlay.
 */
void
mbox_overlay_show(struct mbox_overlay * const inst, int secs);


/**
 * Create an overlay instance.
 */
struct mbox_overlay *
mbox_overlay_new(struct avbox_player * player);


/**
 * Destroy overlay instance.
 */
void
mbox_overlay_destroy(struct mbox_overlay * const inst);


#endif
