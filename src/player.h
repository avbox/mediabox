#ifndef __MB_PLAYER__
#define __MB_PLAYER__

#include "video.h"
#include "input.h"


struct mbp;


/**
 * Media player status enum.
 */
enum mb_player_status
{
        MB_PLAYER_STATUS_READY,
        MB_PLAYER_STATUS_PLAYING,
        MB_PLAYER_STATUS_PAUSED
};


/* status changed callback function */
typedef void (*mb_player_status_callback)(struct mbp *inst, enum mb_player_status status);


/**
 * Get the current status of a media player instance.
 */
enum mb_player_status
mb_player_getstatus(struct mbp* inst);


int
mb_player_add_status_callback(struct mbp *inst, mb_player_status_callback callback);


void
mb_player_update(struct mbp* inst);


int 
mb_player_play(struct mbp* inst, const char * const path);


int
mb_player_pause(struct mbp* inst);


int
mb_player_stop(struct mbp* inst);


/**
 * Create a new media player instance.
 */
struct mbp*
mb_player_new(struct mbv_window *window);


/**
 * Destroy media player instance.
 */
void
mb_player_destroy(struct mbp *inst);

#endif

