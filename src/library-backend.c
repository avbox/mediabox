#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>

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
#define AVMOUNT_BIN "/usr/bin/avmount"
#define AVMOUNT_MOUNTPOINT "/media/UPnP"
#define DEFAULT_LOGFILE "/var/log/avmount-mediabox.log"


LISTABLE_STRUCT(mb_mediatomb_inst,
	int procid;
);


LIST_DECLARE_STATIC(mediatomb_instances);
static int avmount_process_id = -1;


/**
 * Starts a mediatomb instance.
 */
static int
mb_library_backend_startmediatomb(const char * iface_name, void  *data)
{
	struct mb_mediatomb_inst *inst;
	char * const mtargs[] =
	{
		MEDIATOMB_BIN,
		"-e",
		(char*) iface_name,
		"--port",
		"49152",
		"--config",
		MEDIATOMB_RUN "/config.xml",
		NULL
	};

	(void) data;

	/* for now don't launch loopback instance */
	if (!strcmp("lo", iface_name)) {
		return 0;
	}

	if ((inst = malloc(sizeof(struct mb_mediatomb_inst))) == NULL) {
		LOG_VPRINT_ERROR("Could not allocate memory for mediatomb instance %s",
			iface_name);
		return -1;
	}

	/* launch the mediatomb process */
	if ((inst->procid = mb_process_start(MEDIATOMB_BIN, mtargs,
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
 * Initialize mediabox config files
 */
static int
mb_library_backend_mediaboxsetup(const char * const template_path)
{
	DEBUG_VPRINT("library-backend", "Mediatomb setup from: %s",
		template_path);

	/* create mediatomb runtime directory */
	if (mkdir_p(MEDIATOMB_RUN, S_IRWXU | S_IRWXG) == -1) {
		LOG_PRINT_ERROR("Could not create mediatomb runtime directory!");
		return -1;
	}

	/* copy config files */
	if (mb_library_backend_configcp(template_path, "config.xml") != 0 ||
		mb_library_backend_configcp(template_path, "common.js") != 0 ||
		mb_library_backend_configcp(template_path, "import.js") != 0 ||
		mb_library_backend_configcp(template_path, "playlists.js") != 0) {
		return -1;
	}
	return 0;
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
				if (mb_library_backend_mediaboxsetup(conf_path) != 0) {
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
		if (mb_library_backend_mediaboxsetup(DATADIR "/mediabox/mediatomb") != 0) {
			LOG_PRINT(LOGLEVEL_ERROR, "library-backend",
				"Could not setup mediatomb config (2).");
			return -1;
		}
	}


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
		ifaceutil_enumifaces(mb_library_backend_startmediatomb, NULL);
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
			"49153",
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
			"-p 49153 -o allow_other " AVMOUNT_MOUNTPOINT, avmount_logfile);

		if ((avmount_process_id = mb_process_start(AVMOUNT_BIN, avargs,
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
