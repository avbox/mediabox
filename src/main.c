/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <sys/types.h>
#include <asoundlib.h>

#define LOG_MODULE "main"

#include "video.h"
#include "input.h"
#include "timers.h"
#include "player.h"
#include "shell.h"
#include "process.h"
#include "su.h"
#include "library-backend.h"
#include "downloads-backend.h"
#include "announce.h"
#include "debug.h"
#include "log.h"
#include "sysinit.h"

#ifdef ENABLE_IONICE
#include "ionice.h"
#endif

#define WORKDIR  "/var/lib/mediabox"

const char *prog;

/**
 * Signal handler
 */
static void
signal_handler(int signum)
{
	switch (signum) {
	case SIGINT:
	case SIGHUP:
	case SIGTERM:
	{
		int queue;
		DEBUG_PRINT("main", "Received SIGTERM");
		if ((queue = mbs_getqueue()) != -1) {
			mbi_sendmessage(queue, MBI_EVENT_QUIT, NULL, 0);
		} else {
			LOG_PRINT_ERROR("Could not get the shell's message queue");
		}
		break;
	}
	default:
		DEBUG_VPRINT("main", "Received signal: %i", signum);
		break;
	}
}


/**
 * Print usage.
 */
static void
print_usage()
{
	printf("%s: mediabox [options]\n", prog);
	printf("\n");
	printf(" --version\t\tPrint version information\n");
	printf(" --no-direct\t\tDon't use direct rendering\n");
	printf(" --dont-launch-avmount\tAssume avmount is already mounted\n");
	printf(" --dont-launch-mediatomb\tDon't launch mediatomb\n");
	printf(" --dfb:XXX\t\tDirectFB options. See directfbrc(5)\n");
	printf(" --logfile\t\tLog file\n");
	printf(" --help\t\t\tShow this help\n");
	printf("\n");
}


int
main (int argc, char **argv)
{
	int i;
	int launch_avmount = 1;
	int launch_mediatomb = 1;
	int init;
	char *progmem, *logfile = NULL;

	init = (getpid() == 1);

	/* save program name */
	if ((progmem = strdup(argv[0])) == NULL) {
		prog = argv[0];
	} else {
		prog = basename(progmem);
	}

	/* parse command line */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-direct")) {
			fprintf(stderr, "main: Disabling of direct rendering not implemented.\n");

		} else if (!strcmp(argv[i], "--help")) {
			print_usage();
			exit(EXIT_SUCCESS);

		} else if (!strcmp(argv[i], "--version")) {
			fprintf(stdout, "MediaBox Version " PACKAGE_VERSION "\n");
			fprintf(stdout, "Copyright (c) 2016 Fernando Rodriguez. All rights reserved.\n");
			exit(EXIT_SUCCESS);
		} else if (!memcmp(argv[i], "--dfb:", 6)) {
			/* let dfb args pass */
		} else if (!strcmp(argv[i], "--dont-launch-avmount")) {
			launch_avmount = 0;
		} else if (!strcmp(argv[i], "--dont-launch-mediatomb")) {
			launch_mediatomb = 0;
		} else if (!strcmp(argv[i], "--init")) {
			init = 1;
		} else if (!strcmp(argv[i], "--logfile")) {
			logfile = argv[++i];
		} else {
			fprintf(stderr, "main: Invalid argument %s\n", argv[i]);
			print_usage();
			exit(EXIT_FAILURE);
		}
	}

	/* initialize logging system for early
	 * logging */
	log_init();

	/* if not logfile was specified and we're running
	 * as init then use the default */
	if (init && logfile == NULL) {
		logfile = "/var/log/mediabox.log";
	}

	/* Change the working directory.
	 * This is where profiling data will be stored */
	if (chdir(WORKDIR) == -1) {
		LOG_VPRINT_ERROR("Could not change directory: %s",
			strerror(errno));
	}

	/* initialize timers system */
	if (mbt_init() != 0) {
		fprintf(stderr, "Could not initialize timers system. Exiting.\n");
		exit(EXIT_FAILURE);
	}

	/* initialize process manager */
	if (mb_process_init() != 0) {
		fprintf(stderr, "Could not initialize daemons launcher. Exiting.\n");
		exit(EXIT_FAILURE);
	}

	/* initialize video device */
	mbv_init(argc, argv);

	/* initialize system */
	if (init && sysinit_init(logfile) != 0) {
		LOG_PRINT_ERROR("Could not initialize system");
		exit(EXIT_FAILURE);
	}

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

	/* initialize the shell */
	if (mbs_init() != 0) {
		fprintf(stderr, "Could not initialize shell\n");
		exit(EXIT_FAILURE);
	}

	/* initialize the library backend */
	if (mb_library_backend_init(launch_avmount, launch_mediatomb) == -1) {
		fprintf(stderr, "Could not initialize library backend\n");
		exit(EXIT_FAILURE);
	}

	if (mb_downloadmanager_init() == -1) {
		fprintf(stderr, "Could not initialize download manager\n");
		exit(EXIT_FAILURE);
	}

	/* initialize the discovery service */
	if (mb_announce_start() == -1) {
		fprintf(stderr, "Could not start announcer.\n");
		mb_downloadmanager_destroy();
		exit(EXIT_FAILURE);
	}

	/* register signal handlers */
	if (signal(SIGTERM, signal_handler) == SIG_ERR ||
		signal(SIGHUP, signal_handler) == SIG_ERR ||
		signal(SIGINT, signal_handler) == SIG_ERR) {
		LOG_PRINT_ERROR("Could not set signal handlers");
		exit(EXIT_FAILURE);
	}

	/* show the shell */
	mbs_showdialog();

	/* cleanup */
	mb_announce_stop();
	mb_downloadmanager_destroy();
	mbs_destroy();

	mb_process_shutdown();
	mbt_shutdown();
	mbi_destroy();
	mbv_destroy();

	snd_config_update_free_global();

	if (progmem != NULL) {
		free(progmem);
	}

	return 0;
}

