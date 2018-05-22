#ifndef __AVBOX_TORRENTIN__
#define __AVBOX_TORRENTIN__

#include <stdint.h>
#include <libavformat/avio.h>

#include "avbox.h"


/**
 * Opens a DVD device for reading.
 */
struct avbox_player_stream *
avbox_torrentin_open(const char * const path,
	struct avbox_player * const player,
	struct avbox_player_stream * const stream);

#endif
