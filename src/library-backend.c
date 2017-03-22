/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libgen.h>
#include <assert.h>

#define LOG_MODULE "library-backend"

#include "library-backend.h"
#include "process.h"
#include "debug.h"
#include "log.h"
#include "iface_util.h"
#include "linkedlist.h"
#include "proc_util.h"
#include "file_util.h"


#define MEDIATOMB_BIN "/usr/bin/mediatomb"
#define MEDIATOMB_RUN "/tmp/mediabox/mediatomb"
#define MEDIATOMB_VAR "/var/mediabox/mediatomb"
#define AVMOUNT_BIN "/usr/bin/avmount"
#define AVMOUNT_MOUNTPOINT "/media/UPnP"
#define DEFAULT_LOGFILE "/var/log/avmount-mediabox.log"


LISTABLE_STRUCT(mb_mediatomb_inst,
	int procid;
);

struct mt_init_state
{
	int port;
	int err;
	char *home;
};

LIST_DECLARE_STATIC(mediatomb_instances);
static int avmount_process_id = -1;


/**
 * Starts a mediatomb instance.
 */
static int
mb_library_backend_startmediatomb(const char * iface_name, void  *data)
{
	struct mb_mediatomb_inst *inst;
	struct mt_init_state *state = (struct mt_init_state*) data;
	char port[6];
	char homedir[128];
	char * mtargs[] =
	{
		MEDIATOMB_BIN,
		"-e",
		(char*) iface_name,
		"--port",
		port,
		"--config",
		MEDIATOMB_RUN "/config.xml",
		"--home",
		homedir,
		NULL
	};

	assert(data != NULL);
	assert(state->port > 0 && state->port < 65536);

	/* if we errored out on a previous iteration
	 * then exit */
	if (state->err != 0) {
		return 0;
	}

	/* fill the dynamic arguments (port # and homedir) as
	 * every mt instance must run in it's own home directory
	 * and port. Also create the home directory if it doesn't
	 * already exists */
	snprintf(port, sizeof(port), "%i", state->port++);
	snprintf(homedir, sizeof(homedir), "%s.%s", state->home, iface_name);
	if (mkdir_p(homedir, S_IRWXU | S_IRWXG) == -1) {
		state->err = 1;
		return 0;
	}

	/* for now don't launch loopback instance */
	if (!strcmp("lo", iface_name)) {
		mtargs[6] = MEDIATOMB_RUN "/config-local.xml";
	}

	if ((inst = malloc(sizeof(struct mb_mediatomb_inst))) == NULL) {
		LOG_VPRINT_ERROR("Could not allocate memory for mediatomb instance %s",
			iface_name);
		return -1;
	}

	/* launch the mediatomb process */
	if ((inst->procid = mb_process_start(MEDIATOMB_BIN, (const char **) mtargs,
		MB_PROCESS_AUTORESTART | MB_PROCESS_NICE | MB_PROCESS_IONICE_IDLE |
		MB_PROCESS_SUPERUSER/* | MB_PROCESS_STDOUT_LOG | MB_PROCESS_STDERR_LOG */,
		"mediatomb", NULL, NULL)) == -1) {
		LOG_PRINT_ERROR("Could not start mediatomb daemon");
		free(inst);
		return -1;
	}

	LIST_ADD(&mediatomb_instances, inst);

	return 0;
}


/**
 * Copies a mediatomb config file to the runtime
 * directory.
 */
static int
mb_library_backend_configcp(const char * const template_path,
	const char * const filename)
{
	char src_path[255];
	char dst_path[255];

	snprintf(src_path, sizeof(src_path), "%s/%s", template_path, filename);
	snprintf(dst_path, sizeof(dst_path), MEDIATOMB_RUN "/%s", filename);

	/* copy the file */
	if (cp(src_path, dst_path) == -1) {
		LOG_VPRINT(LOGLEVEL_ERROR, "library-backend",
			"Could not copy '%s' to '%s'!", src_path, dst_path);
		return -1;
	}

	return 0;
}


/**
 * Write a random generate UUID string to the
 * buffer. The buffer needs to be 37 characters
 * long.
 */
static char *
getuuidstring(char * const buf)
{
	int fd;
	assert(buf != NULL);
	if ((fd = open("/proc/sys/kernel/random/uuid", O_RDONLY, NULL)) == -1) {
		LOG_VPRINT_ERROR("Could not open '/proc/sys/kernel/random/uuid': %s",
			strerror(errno));
		return NULL;
	}
	if (read(fd, buf, 36) != 36) {
		LOG_VPRINT_ERROR("Could not read uuid!: %s",
			strerror(errno));
		close(fd);
		return NULL;
	}
	close(fd);
	buf[36] = '\0';
	return buf;
}


/**
 * Initialize mediabox config files
 */
static char *
mb_library_backend_mediaboxsetup(const char * const template_path)
{
	int fd;
	char * ret = NULL;
	char * udnfile = NULL;
	char * mediatomb_home = NULL;
	char mediatomb_udn[37] = "", hostname[36] = "";

	DEBUG_VPRINT("library-backend", "Mediatomb setup from: %s",
		template_path);

	/* create mediatomb runtime directory */
	if (mkdir_p(MEDIATOMB_RUN, S_IRWXU | S_IRWXG) == -1) {
		LOG_PRINT_ERROR("Could not create mediatomb runtime directory!");
		goto end;
	}

	/* create the home directory */
	if (mkdir_p(MEDIATOMB_VAR, S_IRWXU | S_IRWXG) == -1) {
		LOG_PRINT_ERROR("Could not create mediatomb variable directory!");
	} else {
		if ((mediatomb_home = strdup(MEDIATOMB_VAR)) == NULL) {
			LOG_PRINT_ERROR("Could not allocate memory for homedir string!!");
		}
	}

	/* if the home directory is unaccessible use the user's
	 * home directory */
	if (mediatomb_home == NULL) {
		#define CONFIGDIR "/.mediabox/mediatomb"

		char * const homedir = getenv("HOME");
		char * configdir = NULL;

		if (homedir != NULL) {
			if ((configdir = malloc(strlen(homedir) + sizeof(CONFIGDIR))) == NULL) {
				LOG_PRINT_ERROR("Could not allocate memory for homedir string!!!");
			} else {
				strcpy(configdir, homedir);
				strcat(configdir, CONFIGDIR);
				DEBUG_VPRINT("library-backend", "Attempting to create '%s'",
					configdir);
				if (mkdir_p(configdir, S_IRWXU | S_IRWXG) == -1) {
					LOG_PRINT_ERROR("Could not create mediatomb variable directory!!");
					free(configdir);
				} else {
					mediatomb_home = configdir;
				}
			}
		}
	}

	/* if at this point we didn't find a home directory
	 * then we're out of luck */
	if (mediatomb_home == NULL) {
		LOG_PRINT_ERROR("Could not find a suitable home directory!");
		goto end;
	}

	DEBUG_VPRINT("library-backend", "Using mediatomb homedir: %s",
		mediatomb_home);

	/* copy config files */
	if (mb_library_backend_configcp(template_path, "config.xml") != 0 ||
		mb_library_backend_configcp(template_path, "common.js") != 0 ||
		mb_library_backend_configcp(template_path, "import.js") != 0 ||
		mb_library_backend_configcp(template_path, "playlists.js") != 0) {
		goto end;
	}

	/* compose udn filepath */
	if ((udnfile = malloc((strlen(mediatomb_home) + 5) * sizeof(char))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate memory for udn filepath!");
		goto end;
	}
	strcpy(udnfile, mediatomb_home);
	strcat(udnfile, "/udn");

	DEBUG_VPRINT("library-backend", "Attempting to open %s",
		udnfile);

	/* try to read the udn file (if it exists) */
	if ((fd = open(udnfile, O_RDONLY, NULL)) == -1) {
		if (errno != ENOENT) {
			LOG_VPRINT_ERROR("Could not open %s for read access: %s!",
				udnfile, strerror(errno));
			goto end;
		}
	} else {
		if (read(fd, mediatomb_udn, 36) == -1) {
			LOG_VPRINT_ERROR("Could not read %s: %s!!",
				udnfile, strerror(errno));
			goto end;
		}
		close(fd);
	}

	/* if there's no udn file generate one */
	if (*mediatomb_udn == '\0') {
		DEBUG_VPRINT("library-backend", "Creating %s",
			udnfile);
		if (getuuidstring(mediatomb_udn) == NULL) {
			LOG_PRINT_ERROR("Could not generate UUID!!");
			goto end;
		}
		DEBUG_VPRINT("library-backend", "New uuid: %s",
			mediatomb_udn);
		if ((fd = open(udnfile, O_CREAT | O_RDWR, S_IRWXU | S_IRWXG)) == -1) {
			LOG_VPRINT_ERROR("Could not create '%s': %s!",
				udnfile, strerror(errno));
			goto end;
		}
		assert(strlen(mediatomb_udn) == 36);
		if (write(fd, mediatomb_udn, 36) == -1) {
			LOG_VPRINT_ERROR("Could not save udn file %s. Continuing.",
				udnfile);
		}
		close(fd);
	}

	/* get the system's hostname */
	if (gethostname(hostname, sizeof(hostname)) == -1) {
		LOG_VPRINT_ERROR("Could not get hostname: %s",
			strerror(errno));
	}

	DEBUG_VPRINT("library-backend", "System hostname is '%s'",
		hostname);

	const char * const match[] =
	{
		"@HOMEDIR@",
		"@UDN@",
		"@HOSTNAME@",
		"@ENABLEUI@",
		NULL
	};
	const char * replace[] =
	{
		mediatomb_home,
		mediatomb_udn,
		hostname,
		"no",
		NULL
	};

	/* now that we have the configuration files
	 * on our home directory we need to edit them. */
	if (frep(MEDIATOMB_RUN "/config.xml",
		MEDIATOMB_RUN "/config-local.xml",
		match, replace) != 0) {
		LOG_PRINT_ERROR("Could not update config-local.xml!!");
		goto end;
	}

	replace[3] = "yes"; /* @ENABLEUI@ */

	if (frep(MEDIATOMB_RUN "/config.xml",
		MEDIATOMB_RUN "/config.xml",
		match, replace) != 0) {
		LOG_PRINT_ERROR("Could not update config.xml!!");
		goto end;
	}

	ret = mediatomb_home;
end:
	if (udnfile != NULL) {
		free(udnfile);
	}
	if (ret == NULL && mediatomb_home != NULL) {
		free(mediatomb_home);
	}
	return ret;
}


/**
 * Initialize the library backend.
 */
int
mb_library_backend_init(const int launch_avmount,
	const int launch_mediatomb)
{
	char exe_path_mem[255];
	char *exe_path = exe_path_mem;
	char *avmount_logfile = NULL;
	char *mt_home = NULL;
	int config_setup = 0;
	struct stat st;

	DEBUG_PRINT("library-backend", "Starting library backend");

	/* Figure out which config file template to use.
	 * If we're running on the build directory we use
	 * the ones in the source tree. Otherwise use the
	 * installed ones */
	if (mb_getexepath(exe_path, sizeof(exe_path_mem)) == -1) {
		LOG_VPRINT(LOGLEVEL_ERROR, "library-backend", "Could not get executable path: %s",
			strerror(errno));
		exe_path[0] = '\0';

	} else {
		char *conf_path;
		exe_path = dirname(exe_path);

		DEBUG_VPRINT("library-backend", "Executable image path: %s",
			exe_path);

		if ((conf_path = malloc((strlen(exe_path) + 25 + 1) * sizeof(char))) != NULL) {
			struct stat st;
			strcpy(conf_path, exe_path);
			strcat(conf_path, "/res/mediatomb/config.xml");
			if (stat(conf_path, &st) == 0) {
				DEBUG_VPRINT("library-backend", "Config template found at: %s",
					conf_path);
				strcpy(conf_path, exe_path);
				strcat(conf_path, "/res/mediatomb");
				if ((mt_home = mb_library_backend_mediaboxsetup(conf_path)) == NULL) {
					LOG_PRINT(LOGLEVEL_ERROR, "library-backend",
						"Could not setup mediatomb config.");
					return -1;
				}
				config_setup = 1;
			} else {
				DEBUG_VPRINT("library-backend", "Config template not found: %s",
					conf_path);
			}
			free(conf_path);
		}
	}

	if (!config_setup) {
		if ((mt_home = mb_library_backend_mediaboxsetup(DATADIR "/mediabox/mediatomb")) == NULL) {
			LOG_PRINT(LOGLEVEL_ERROR, "library-backend",
				"Could not setup mediatomb config (2).");
			return -1;
		}
	}

	assert(mt_home != NULL);

	/* check that we have permission to write to DEFAULT_LOGFILE
	 * before trying */
	if (stat(DEFAULT_LOGFILE, &st) == 0) {
		if (access(DEFAULT_LOGFILE, W_OK) == 0) {
			avmount_logfile = DEFAULT_LOGFILE;
		}
	} else {
		if (stat("/var/log", &st) == 0) {
			if (access("/var/log", W_OK) == 0) {
				avmount_logfile = DEFAULT_LOGFILE;
			}
		}
	}

	/* If we can't write to the log file (probably because
	 * we're running as a regular user for debugging) then
	 * write the log to /tmp */
	if (avmount_logfile == NULL) {
		avmount_logfile = "/tmp/avmount-mediabox.log";
	}

	/* initialize a linked list to hold mediatomb instances */
	LIST_INIT(&mediatomb_instances);

	/* launch a mediabox process for each interface */
	if (launch_mediatomb) {
		struct mt_init_state state;
		state.port = 49163;
		state.home = mt_home;
		state.err = 0;
		ifaceutil_enumifaces(mb_library_backend_startmediatomb, &state);
		free(mt_home);
		if (state.err != 0) {
			LOG_PRINT_ERROR("An error occurred while launching mediatomb!");
			return -1;
		}
	}

	/* launch the avmount process */
	if (launch_avmount) {
		struct stat st;

		char * const avargs[] =
		{
			AVMOUNT_BIN,
			"-l",
			avmount_logfile,
			"--lobind",
			"-f",
			"-p",
			"49152",
			"-o",
			"allow_other",
			AVMOUNT_MOUNTPOINT,
			NULL
		};

		/* if the mountpoint doesn't exist, create it */
		if (stat(AVMOUNT_MOUNTPOINT, &st)) {
			DEBUG_PRINT("library-backend", AVMOUNT_MOUNTPOINT " not found.Creating");
			if (mkdir_p(AVMOUNT_MOUNTPOINT, S_IRWXU)) {
				DEBUG_VPRINT("library-backend", "Could not create mountpoint "
					AVMOUNT_MOUNTPOINT ": %s", strerror(errno));
			}
			return -1;
		}


		DEBUG_VPRINT("library-backend", "Running " AVMOUNT_BIN " -l %s --lobind -f "
			"-p 49152 -o allow_other " AVMOUNT_MOUNTPOINT, avmount_logfile);

		if ((avmount_process_id = mb_process_start(AVMOUNT_BIN, (const char **) avargs,
			MB_PROCESS_AUTORESTART | MB_PROCESS_NICE | MB_PROCESS_IONICE_IDLE |
			MB_PROCESS_SUPERUSER | MB_PROCESS_STDOUT_LOG | MB_PROCESS_STDERR_LOG,
			"avmount", NULL, NULL)) == -1) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "library-backend", "Could not start avmount daemon");
			return -1;
		}
	}

	return 0;
}


/**
 * mb_library_backend_shutdown() -- Shutdown the library backend.
 */
void
mb_library_backend_shutdown(void)
{
	struct mb_mediatomb_inst *inst;
	LIST_FOREACH_SAFE(struct mb_mediatomb_inst*, inst, &mediatomb_instances, {
		mb_process_stop(inst->procid);
		LIST_REMOVE(inst);
	});
	if (avmount_process_id != -1) {
		mb_process_stop(avmount_process_id);
	}
}
