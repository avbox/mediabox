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
#include <sys/mount.h>
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
#include "settings.h"


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
		if ((queue = avbox_shell_getqueue()) != -1) {
			avbox_input_sendmessage(queue, MBI_EVENT_QUIT, NULL, 0);
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
 * Parse kernel arguments
 */
static char **
parse_kernel_args(int *argc)
{
#define CMDLINE_MAX	(1024)
#define ARGS_MAX	(10)

	int fd, i = 0, n = 0;
	char buf[CMDLINE_MAX];
	char **argv, *arg;
	ssize_t nb_read;

	/* we need to make sure /proc is mounted before we
	 * can read /proc/cmdline */
	if (mount("proc", "/proc", "proc", 0, "") == -1) {
		return NULL;
	}

	/* read the file */
	if ((fd = open("/proc/cmdline", O_RDONLY)) == -1) {
		return NULL;
	}
	if ((nb_read = read(fd, buf, CMDLINE_MAX-2)) < 0 || nb_read  == CMDLINE_MAX-2) {
		close(fd);
		if (nb_read == CMDLINE_MAX-2) {
			errno = E2BIG;
		}
		return NULL;
	}
	close(fd);

	/* make sure command-line is NULL terminate and replace
	 * the ending newline with a space so we terminate the
	 * last argument before exitting the loop */
	assert(nb_read >= 1 && buf[nb_read - 1] == '\n');
	buf[nb_read - 1] = ' '; /* ensure we're closed at end of loop */
	buf[nb_read + 0] = '\0';

	/* allocate heap memory for arguments list and the
	 * first argument (program name) */
	if ((argv = malloc((ARGS_MAX+1) * sizeof(char*))) == NULL) {
		return NULL;
	} else {
		memset(argv, 0, (ARGS_MAX+1) * sizeof(char*));
	}
	if ((argv[0] = strdup(prog)) == NULL) {
		return NULL;
	}
	*argc = 1;

	/* parse all arguments prefixed with 'mediabox.' */
	while (i < nb_read) {
		if (buf[i] != ' ' && buf[i] != '\n') {
			if (n == 0) {
				arg = &buf[i];
			}
			n++;
		} else {
			buf[i] = '\0';
			if (n > 0) {
				if (*argc < ARGS_MAX && !strncmp("mediabox.", arg, 9)) {
					if ((argv[*argc] = malloc((n - 6) * sizeof(char))) == NULL) {
						goto err;
					}

					sprintf(argv[*argc], "--%s", arg + 9);
					assert((n - 6) > strlen(argv[*argc]));
					(*argc)++;
				} else {
					if (*argc >= ARGS_MAX) {
						fprintf(stderr, "%s: Too many arguments! This build "
							"only supports %d kernel arguments!!",
							prog, ARGS_MAX);
						errno = E2BIG;
						goto err;
					}
				}
				n = 0;
			}
		}
		i++;
	}

	return argv;
err:
	for (i = 0; i < ARGS_MAX; i++) {
		if (argv[i] != NULL) {
			free(argv[i]);
		}
	}
	free(argv);
	return NULL;

#undef ARGS_MAX
#undef CMDLINE_MAX
}


/**
 * Free the list of kernel arguments.
 */
static void
free_kernel_args(int argc, char **argv)
{
	int i;
	for (i = 0; i < argc; i++) {
		assert(argv[i] != NULL);
		free(argv[i]);
	}
	assert(argv[i] == NULL);
	free(argv);
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
	printf(" --video:driver=<drv>\tSet the video driver string\n");
	printf(" --dont-launch-avmount\tAssume avmount is already mounted\n");
	printf(" --dont-launch-mediatomb\tDon't launch mediatomb\n");
	printf(" --dfb:XXX\t\tDirectFB options. See directfbrc(5)\n");
	printf(" --logfile\t\tLog file\n");
	printf(" --help\t\t\tShow this help\n");
	printf("\n");
}


int
main (int argc, char **cargv)
{
	int i;
	int launch_avmount = 1;
	int launch_mediatomb = 1;
	int init;
	char *progmem, *logfile = NULL;
	char **argv;

	init = (getpid() == 1);

	/* save program name */
	if ((progmem = strdup(cargv[0])) == NULL) {
		prog = cargv[0];
	} else {
		prog = basename(progmem);
	}

	/* if we're running as pid 1 parse arguments from
	 * kernel */
	if (init) {
		if ((argv = parse_kernel_args(&argc)) == NULL) {
			fprintf(stderr, "%s: Cannot parse kernel args: %s (%i)",
				prog, strerror(errno), errno);
			exit(EXIT_FAILURE);
		}
	} else {
		argv = cargv;
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
		} else if (!strncmp(argv[i], "--video:", 8)) {
			/* let video args pass */
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

	/* if no logfile was specified and we're running
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

	/* initialize settings database */
	if (avbox_settings_init() == -1) {
		fprintf(stderr, "Could not initialize settings database!\n");
		exit(EXIT_FAILURE);
	}

	/* initialize timers system */
	if (avbox_timers_init() != 0) {
		fprintf(stderr, "Could not initialize timers system. Exiting.\n");
		exit(EXIT_FAILURE);
	}

	/* initialize process manager */
	if (avbox_process_init() != 0) {
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

	DEBUG_PRINT("main", "Initializing input system");

	/* initialize input system */
	if (avbox_input_init() != 0) {
		fprintf(stderr, "Could not initialize input device(s)\n");
		exit(EXIT_FAILURE);
	}

#ifdef ENABLE_IONICE
	if (ioprio_set(IOPRIO_WHO_PROCESS, getpid(), IOPRIO_PRIO_VALUE(IOPRIO_CLASS_RT, 4)) == -1) {
		fprintf(stderr, "main: WARNING: Could not set IO priority to realtime!!\n");
	}
#endif

	/* drop root prividges after initializing framebuffer */
	avbox_droproot();

	/* initialize the shell */
	if (avbox_shell_init() != 0) {
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
	if (avbox_discovery_init() == -1) {
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

	/* if we're running as pid 1 free the list
	 * of kernel arguments */
	if (init) {
		free_kernel_args(argc, argv);
	}

	/* show the shell */
	avbox_shell_run();

	/* cleanup */
	avbox_discovery_shutdown();
	mb_downloadmanager_destroy();
	avbox_shell_shutdown();
	avbox_process_shutdown();
	avbox_timers_shutdown();
	avbox_settings_shutdown();
	avbox_input_shutdown();
	mbv_destroy();

	snd_config_update_free_global();

	if (progmem != NULL) {
		free(progmem);
	}

	return 0;
}

