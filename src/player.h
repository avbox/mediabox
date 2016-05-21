#ifndef __MB_PLAYER__
#define __MB_PLAYER__

#include "input.h"

/* abstract type to represent a player instance */
struct mbp;

int 
mbp_play(struct mbp* inst, const char * const path);

int
mbp_stop(struct mbp* inst);

struct mbp*
mbp_init(void);

void
mbp_destroy(struct mbp *inst);

#endif

