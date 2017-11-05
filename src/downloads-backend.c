/**
 * MediaBox - Linux based set-top firmware
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
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
#include "lib/ionice.h"
#endif
#include "lib/process.h"
#include "lib/debug.h"
#include "lib/log.h"
#include "lib/file_util.h"

#define DELUGE_BIN "/usr/bin/deluge-console"
#define DELUGED_BIN "/usr/bin/deluged"
#define PREFIX "/usr/local"

#define STRINGIZE2(x)	#x
#define STRINGIZE(x)	STRINGIZE2(x)


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
	if ((process_id = avbox_process_start(DELUGE_BIN, (const char **) args,
		AVBOX_PROCESS_NICE | AVBOX_PROCESS_IONICE_IDLE |
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT,
		"deluge-console", NULL, NULL)) == -1) {
		LOG_VPRINT(MB_LOGLEVEL_ERROR, "download-backend",
			"Could not execute deluge-console (errno=%i)", errno);
		return -1;
	}

	/* wait for process to exit */
	avbox_process_wait(process_id, &exit_status);

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
	cp(STRINGIZE(DATADIR) "/mediabox/deluge/core.conf", "/tmp/mediabox/deluge/core.conf");
	cp(STRINGIZE(DATADIR) "/mediabox/deluge/auth", "/tmp/mediabox/deluge/auth");
	unlink("/tmp/mediabox/deluge/deluged.pid");

	/* launch the deluged process */
	if ((daemon_id = avbox_process_start(DELUGED_BIN, (const char **) args,
		AVBOX_PROCESS_AUTORESTART | AVBOX_PROCESS_NICE | AVBOX_PROCESS_IONICE_IDLE | AVBOX_PROCESS_SUPERUSER,
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
		avbox_process_stop(daemon_id);
	}
}
