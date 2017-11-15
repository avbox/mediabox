#ifndef __AVBOX_DVDIO__
#define __AVBOX_DVDIO__

#include <stdint.h>
#include <libavformat/avio.h>
#include <dvdnav/dvdnav.h>

struct avbox_dvdio;


typedef void (*avbox_dvdio_dvdnavcb)(void *context, int event, void *data);


/**
 * Opens a DVD device for reading.
 */
struct avbox_dvdio *
avbox_dvdio_open(const char * const path,
	avbox_dvdio_dvdnavcb callback, void * callback_context);


/**
 * Wake any threads waiting on IO
 */
void
avbox_dvdio_wake(struct avbox_dvdio * const inst);


/**
 * Start playing the DVD
 */
void
avbox_dvdio_play(struct avbox_dvdio * const inst);


/**
 * Sets non-blocking mode
 */
int
avbox_dvdio_isblocking(struct avbox_dvdio * const inst);


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

/**
 * Gets the dvdnav instance.
 */
dvdnav_t *
avbox_dvdio_dvdnav(struct avbox_dvdio * const inst);


#endif
