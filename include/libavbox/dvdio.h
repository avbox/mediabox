#ifndef __AVBOX_DVDIO__
#define __AVBOX_DVDIO__

#include <stdint.h>
#include <libavformat/avio.h>

#include "avbox.h"


/**
 * Opens a DVD device for reading.
 */
struct avbox_player_stream *
avbox_dvdio_open(const char * const path,
	struct avbox_player * const player,
	struct avbox_player_stream * const stream);

#endif
