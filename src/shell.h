#ifndef __MB_SHELL_H__
#define __MB_SHELL_H__


/**
 * mbs_get_active_player() -- Gets the currently active player instance.
 */
struct mbp *
mbs_get_active_player(void);


/**
 * mbs_init() -- Initialize the MediaBox shell
 */
int
mbs_init(void);


int
mbs_show_dialog(void);


void
mbs_destroy(void);

#endif
