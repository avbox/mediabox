#ifndef __AVBOX_DVDIO__
#define __AVBOX_DVDIO__

#include <stdint.h>
#include <libavformat/avio.h>
#include <dvdnav/dvdnav.h>

#include "avbox.h"

struct avbox_dvdio;


/**
 * Opens a DVD device for reading.
 */
struct avbox_dvdio *
avbox_dvdio_open(const char * const path,
	struct avbox_player * const player);

/**
 * Gets the coordinates of the highlighted item.
 */
struct avbox_rect*
avbox_dvdio_highlight(struct avbox_dvdio * const inst);


/**
 * Gets the underlying object.
 */
struct avbox_object *
avbox_dvdio_object(struct avbox_dvdio * const inst);


/**
 * Returns 1 if the stream can be paused.
 */
int
avbox_dvdio_canpause(const struct avbox_dvdio * const inst);


/**
 * Seek the stream
 */
void
avbox_dvdio_seek(struct avbox_dvdio * const inst, int flags, int64_t pos);


/**
 * Reopen a closed dvdio stream.
 */
int
avbox_dvdio_reopen(struct avbox_dvdio * const inst);


/**
 * Wake any threads waiting on IO
 */
void
avbox_dvdio_wake(struct avbox_dvdio * const inst);


/**
 * Start playing the DVD
 */
void
avbox_dvdio_play(struct avbox_dvdio * const inst, const int skip_to_menu);


/**
 * Sets non-blocking mode
 */
int
avbox_dvdio_isblocking(struct avbox_dvdio * const inst);


/**
 * Get the dvdnav stream id that matches for
 * a given AVStream id.
 */
int
avbox_dvdio_dvdnavstream(struct avbox_dvdio * const inst, int stream_id);


/**
 * Returns 1 if the stream is expected to underrun (meaning
 * that the player should not handle the underrun), 0
 * otherwise.
 */
int
avbox_dvdio_underrunok(const struct avbox_dvdio * const inst);


/**
 * Start the DVDIO state machine.
 */
void
avbox_dvdio_run(struct avbox_dvdio * const inst);


/**
 * Close the DVD.
 */
void
avbox_dvdio_close(struct avbox_dvdio *inst);


/**
 * Free DVDIO resources
 */
void
avbox_dvdio_destroy(struct avbox_dvdio * const inst);


/**
 * Get an AVIOContext for this opened DVD.
 */
AVIOContext *
avbox_dvdio_avio(struct avbox_dvdio * const inst);

#endif
