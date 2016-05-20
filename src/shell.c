#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "input.h"
#include "player.h"

struct mbs
{
	struct mbi *input;
	struct mbp *player;
};

/**
 * mbs_init() -- Initialize the MediaBox shell
 */
struct mbs*
mbs_init(struct mbi *input)
{
	struct mbs* inst;

	inst = malloc(sizeof(struct mbs));
	if (inst == NULL) {
		fprintf(stderr, "mbp_init() failed -- out of memory\n");
		return NULL;
	}

	inst->input = input;
	inst->player = NULL;
	return inst;
}

int
mbs_show(struct mbs* inst)
{
	int fd;
	mbi_event e;

	if ((fd = mbi_grab_input(inst->input)) == -1) {
		fprintf(stderr, "mbs_show() -- mbi_grab_input failed\n");
		return -1;
	}

	while (read_or_eof(fd, &e, sizeof(mbi_event)) != 0) {
		fprintf(stderr, "mbs: Received event %i\n", (int) e);
	}

	return 0;
}

void
mbs_destroy(struct mbs *inst)
{
	if (inst == NULL) {
		return;
	}
	free(inst);
}

