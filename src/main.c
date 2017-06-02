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

#include "lib/application.h"
#include "shell.h"

#define WORKDIR  "/var/lib/mediabox"

/**
 * Print usage.
 */
static void
print_usage(const char * const prog)
{
	printf("%s: mediabox [options]\n", prog);
	printf("\n");
	printf(" --version\t\tPrint version information\n");
	printf(" --no-avmount\t\tAssume avmount is already mounted\n");
	printf(" --no-mediatomb\t\tDon't launch mediatomb\n");
	printf("\n");
	printf("AVBox options:\n\n");
	printf(" --video:driver=<drv>\tSet the video driver string\n");
	printf(" --dfb:XXX\t\tDirectFB options. See directfbrc(5)\n");
	printf(" --logfile\t\tLog file\n");
	printf(" --help\t\t\tShow this help\n");
	printf("\n");
}


/**
 * Prints version information.
 */
static void
print_version()
{
	fprintf(stdout, PACKAGE_NAME " Version " PACKAGE_VERSION "\n");
	fprintf(stdout, "Copyright (c) 2016-2017 Fernando Rodriguez. All rights reserved.\n");
}


/**
 * Program entry point.
 */
int
main (int argc, char **argv)
{
	int i;
	int launch_avmount = 1;
	int launch_mediatomb = 1;

	/* parse command line */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help")) {
			print_usage(argv[0]);
			exit(EXIT_SUCCESS);
		} else if (!strcmp(argv[i], "--version")) {
			print_version();
			exit(EXIT_SUCCESS);
		} else if (!strncmp(argv[i], "--dfb:", 6)) {
			/* let dfb args pass */
		} else if (!strncmp(argv[i], "--avbox:", 8)) {
			/* let avbox args through */
			if (!strcmp(argv[i], "--avbox:logfile")) {
				/* pass through */
				i++; /* ignore next */
			}
		} else if (!strncmp(argv[i], "--video:", 8)) {
			/* let video args pass */
		} else if (!strncmp(argv[i], "--input:", 8)) {
			/* let input args pass */
		} else if (!strcmp(argv[i], "--no-avmount")) {
			launch_avmount = 0;
		} else if (!strcmp(argv[i], "--no-mediatomb")) {
			launch_mediatomb = 0;
		} else if (!strcmp(argv[i], "--init")) {
			/* pass through */
		} else {
			fprintf(stderr, "%s: Invalid argument %s\n",
				argv[0], argv[i]);
			print_usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	/* Change the working directory.
	 * This is where profiling data will be stored */
	if (chdir(WORKDIR) == -1) {
		fprintf(stderr, "%s: Could not change working directory: %s\n",
			argv[0], strerror(errno));
	}

	/* initialize application */
	if (avbox_application_init(argc, argv, NULL) == -1) {
		fprintf(stderr, "%s: Initialization error!\n",
			argv[0]);
		exit(EXIT_FAILURE);
	}

	/* initialize the shell */
	if (mbox_shell_init(launch_avmount, launch_mediatomb) != 0) {
		fprintf(stderr, "%s: Could not initialize shell\n",
			argv[0]);
		exit(EXIT_FAILURE);
	}

	/* show the shell */
	if (mbox_shell_show() == -1) {
		fprintf(stderr, "Could not show main window!");
		exit(EXIT_FAILURE);
	} 

	/* run the application loop */
	return avbox_application_run();
}
