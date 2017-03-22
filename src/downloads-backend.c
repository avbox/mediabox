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
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#ifdef ENABLE_IONICE
#include "ionice.h"
#endif
#include "process.h"
#include "debug.h"
#include "log.h"
#include "file_util.h"

#define DELUGE_BIN "/usr/bin/deluge-console"
#define DELUGED_BIN "/usr/bin/deluged"
#define PREFIX "/usr/local"


int daemon_id = -1;


/**
 * mb_downloadmanager_addurl() -- Adds a URL to the download queue.
 */
int
mb_downloadmanager_addurl(char *url)
{
	int process_id, exit_status = -1;

	char * const args[] =
	{
		"deluge-console",
		"connect",
		"127.0.0.1",
		"mediabox",
		"mediabox;",
		"add",
		url,
		NULL
	};

	/* launch the deluged process */
	if ((process_id = mb_process_start(DELUGED_BIN, (const char **) args,
		MB_PROCESS_AUTORESTART | MB_PROCESS_NICE | MB_PROCESS_IONICE_IDLE |
		MB_PROCESS_SUPERUSER | MB_PROCESS_WAIT, "deluge-console", NULL, NULL)) == -1) {
		LOG_VPRINT(MB_LOGLEVEL_ERROR, "download-backend",
			"Could not execute deluge-console (errno=%i)", errno);
		return -1;
	}

	/* wait for process to exit */
	mb_process_wait(process_id, &exit_status);

	return exit_status;
}


/**
 * mb_downloadmanager_init() -- Initialize the download manager.
 */
int
mb_downloadmanager_init(void)
{
	char * const args[] =
	{
		"deluged",
		"-d",
		"-p",
		"58846",
		"-c",
		"/tmp/mediabox/deluge/",
		NULL
	};

	DEBUG_PRINT("download-backend", "Initializing download manager");

	/* create all config files for deluged */
	umask(000);

	mkdir_p("/tmp/mediabox/deluge/plugins", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	cp(DATADIR "/mediabox/deluge/deluge-core.conf", "/tmp/mediabox/deluge/core.conf");
	cp(DATADIR "/mediabox/deluge/deluge-auth", "/tmp/mediabox/deluge/auth");
	unlink("/tmp/mediabox/deluge/deluged.pid");

	/* launch the deluged process */
	if ((daemon_id = mb_process_start(DELUGED_BIN, (const char **) args,
		MB_PROCESS_AUTORESTART | MB_PROCESS_NICE | MB_PROCESS_IONICE_IDLE | MB_PROCESS_SUPERUSER,
		"Deluge Daemon", NULL, NULL)) == -1) {
		fprintf(stderr, "download-backend: Could not start deluge daemon\n");
		return -1;
	}
return 0;
}


/**
 * mb_downloadmanager_destroy() -- Shutdown the download manager.
 */
void
mb_downloadmanager_destroy(void)
{
	DEBUG_PRINT("download-backend", "Shutting down download manager");

	if (daemon_id != -1) {
		mb_process_stop(daemon_id);
	}
}
