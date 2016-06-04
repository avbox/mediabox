#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>

#include "video.h"
#include "input.h"
#include "player.h"
#include "shell.h"
#include "su.h"

int
main (int argc, char **argv)
{
	/* initialize video device */
	mbv_init(argc, argv);

	/* initialize input system */
	if (mbi_init() != 0) {
		fprintf(stderr, "Could not initialize input device(s)\n");
		exit(EXIT_FAILURE);
	}

	/* drop root prividges after initializing framebuffer */
	mb_su_droproot();

	/* initialize the shell */
	if (mbs_init() != 0) {
		fprintf(stderr, "Could not initialize shell\n");
		exit(EXIT_FAILURE);
	}

	/* show the shell */
	mbs_show_dialog();

	/* cleanup */
	mbs_destroy();
	mbi_destroy();
	mbv_destroy();

	return 0;
}

