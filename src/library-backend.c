#include <stdlib.h>

#include "library-backend.h"
#include "process.h"
#include "debug.h"
#include "log.h"


#define MEDIATOMB_BIN "/usr/bin/mediatomb"
#define AVMOUNT_BIN "/usr/bin/avmount"


static int avmount_process_id = -1;
static int mediatomb_process_id = -1;


/**
 * mb_library_backend_init() -- Initialize the library backend.
 */
int
mb_library_backend_init(void)
{
	char * const mtargs[] =
	{
		MEDIATOMB_BIN,
		"-e",
		"enp0s15",
		"--port",
		"49152",
		"--config",
		"/etc/mediatomb/config.xml",
		NULL
	};

	char * const avargs[] =
	{
		AVMOUNT_BIN,
		"--lobind",
		"-f",
		"-p",
		"49153",
		"-o",
		"allow_other",
		"/media/UPnP",
		NULL
	};


	DEBUG_PRINT("library-backend", "Starting library backend");

	/* launch the mediatomb process */
	if ((mediatomb_process_id = mb_process_start(MEDIATOMB_BIN, mtargs,
		MB_PROCESS_AUTORESTART | MB_PROCESS_NICE | MB_PROCESS_IONICE_IDLE |
		MB_PROCESS_SUPERUSER | MB_PROCESS_STDOUT_LOG,
		"mediatomb", NULL, NULL)) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "library-backend", "Could not start mediatomb daemon");
		return -1;
	}

	/* launch the avmount process */
	if ((avmount_process_id = mb_process_start(AVMOUNT_BIN, avargs,
		MB_PROCESS_AUTORESTART | MB_PROCESS_NICE | MB_PROCESS_IONICE_IDLE |
		MB_PROCESS_SUPERUSER | MB_PROCESS_STDOUT_LOG,
		"avmount", NULL, NULL)) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "library-backend", "Could not start avmount daemon");
		return -1;
	}

	return 0;
}


/**
 * mb_library_backend_shutdown() -- Shutdown the library backend.
 */
void
mb_library_backend_shutdown(void)
{
	if (mediatomb_process_id != -1) {
		mb_process_stop(mediatomb_process_id);
	}
	if (avmount_process_id != -1) {
		mb_process_stop(avmount_process_id);
	}
}
