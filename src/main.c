#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>

#include "video.h"
#include "input.h"
#include "player.h"
#include "shell.h"
#include "su.h"
#include "downloads-backend.h"


int
main (int argc, char **argv)
{
	int i;

	/* parse command line */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-direct")) {
			fprintf(stderr, "main: Disabling of direct rendering not implemented.\n");

		} else if (!strcmp(argv[i], "--help")) {
			fprintf(stderr, "main: No help available yet.\n");
			exit(EXIT_SUCCESS);

		} else if (!strcmp(argv[i], "--version")) {
			fprintf(stdout, "MediaBox Version " PACKAGE_VERSION "\n");
			fprintf(stdout, "Copyright (c) 2016 Fernando Rodriguez. All rights reserved.\n");
			exit(EXIT_SUCCESS);
		} else if (!memcmp(argv[i], "--dfb:", 6)) {
			/* let dfb args pass */
		} else {
			fprintf(stderr, "main: Invalid argument %s\n", argv[i]);
			exit(EXIT_FAILURE);
		}
	}

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

	if (mb_downloadmanager_init() == -1) {
		fprintf(stderr, "Could not initialize download manager\n");
		exit(EXIT_FAILURE);
	}

	/* show the shell */
	mbs_show_dialog();

	/* cleanup */
	mb_downloadmanager_destroy();
	mbs_destroy();
	mbi_destroy();
	mbv_destroy();

	return 0;
}

