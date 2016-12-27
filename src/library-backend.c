#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "library-backend"

#include "library-backend.h"
#include "process.h"
#include "debug.h"
#include "log.h"
#include "iface_util.h"
#include "linkedlist.h"


#define MEDIATOMB_BIN "/usr/bin/mediatomb"
#define AVMOUNT_BIN "/usr/bin/avmount"

LISTABLE_STRUCT(mb_mediatomb_inst,
	int procid;
);


LIST_DECLARE_STATIC(mediatomb_instances);
static int avmount_process_id = -1;


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
		"/etc/mediatomb/config.xml",
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
 * mb_library_backend_init() -- Initialize the library backend.
 */
int
mb_library_backend_init(const int launch_avmount)
{
	DEBUG_PRINT("library-backend", "Starting library backend");

	/* initialize a linked list to hold mediatomb instances */
	LIST_INIT(&mediatomb_instances);

	/* launch a mediabox process for each interface */
	ifaceutil_enumifaces(mb_library_backend_startmediatomb, NULL);

	/* launch the avmount process */
	if (launch_avmount) {
		char * const avargs[] =
		{
			AVMOUNT_BIN,
			"-l",
			"/var/log/avmount-mediabox.log",
			"--lobind",
			"-f",
			"-p",
			"49153",
			"-o",
			"allow_other",
			"/media/UPnP",
			NULL
		};

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
