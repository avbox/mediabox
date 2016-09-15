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
#include <asoundlib.h>

#include "video.h"
#include "input.h"
#include "timers.h"
#include "player.h"
#include "shell.h"
#include "process.h"
#include "su.h"
#include "downloads-backend.h"
#include "announce.h"
#include "debug.h"

#ifdef ENABLE_IONICE
#include "ionice.h"
#endif


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

	/* initialize process manager */
	if (mb_process_init() != 0) {
		fprintf(stderr, "Could not initialize daemons launcher. Exiting.\n");
		exit(EXIT_FAILURE);
	}

	/* initialize video device */
	mbv_init(argc, argv);

	/* initialize input system */
	if (mbi_init() != 0) {
		fprintf(stderr, "Could not initialize input device(s)\n");
		exit(EXIT_FAILURE);
	}

#ifdef ENABLE_IONICE
	if (ioprio_set(IOPRIO_WHO_PROCESS, getpid(), IOPRIO_PRIO_VALUE(IOPRIO_CLASS_RT, 4)) == -1) {
		fprintf(stderr, "main: WARNING: Could not set IO priority to realtime!!\n");
	}
#endif

	/* drop root prividges after initializing framebuffer */
	mb_su_droproot();

	/* initialize timers system */
	if (mbt_init() != 0) {
		fprintf(stderr, "Could not initialize timers system. Exiting.\n");
		exit(EXIT_FAILURE);
	}

	/* initialize the shell */
	if (mbs_init() != 0) {
		fprintf(stderr, "Could not initialize shell\n");
		exit(EXIT_FAILURE);
	}

	if (mb_downloadmanager_init() == -1) {
		fprintf(stderr, "Could not initialize download manager\n");
		exit(EXIT_FAILURE);
	}

	if (mb_announce_start() == -1) {
		fprintf(stderr, "Could not start announcer.\n");
		mb_downloadmanager_destroy();
		exit(EXIT_FAILURE);
	}

	/* show the shell */
	mbs_show_dialog();

	/* cleanup */
	mb_announce_stop();
	mb_downloadmanager_destroy();
	mbs_destroy();

	mb_process_shutdown();
	mbt_shutdown();
	mbi_destroy();
	mbv_destroy();

	snd_config_update_free_global();

	return 0;
}

