#ifndef __MB_PLAYER__
#define __MB_PLAYER__

#include "input.h"

/* abstract type to represent a player instance */
struct mbp;


enum mb_player_status
{
        MB_PLAYER_STATUS_READY,
        MB_PLAYER_STATUS_PLAYING,
        MB_PLAYER_STATUS_PAUSED
};


enum mb_player_status
mb_player_getstatus(struct mbp* inst);


void
mb_player_update(struct mbp* inst);


int 
mbp_play(struct mbp* inst, const char * const path);


int
mbp_pause(struct mbp* inst);


int
mbp_stop(struct mbp* inst);


struct mbp*
mbp_init(void);


void
mbp_destroy(struct mbp *inst);

#endif

