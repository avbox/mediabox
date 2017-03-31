#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "sysinit"

/* TODO: Do this at configure time */
#define LINUX 1

#include "debug.h"
#include "log.h"
#include "process.h"
#include "file_util.h"


static int proc_dropbear = -1;
static int proc_dbus = -1;
static int proc_getty = -1;


/**
 * Takes in an executable name and a variable list
 * of of char * arguments terminated by a NULL string
 * pointer and executes and waits for the command to
 * exit and returns the process' exit code.
 *
 * NOTE: If no arguments are needed you must still
 * pass the NULL terminator argument!
 */
static int
sysinit_execargs(const char * const filepath, ...)
{
	int i, proc_tmp, ret_tmp = -1;
	const char *args[8] = { filepath, NULL };
	va_list va;

	va_start(va, filepath);

	for (i = 1; i < 8; i++) {
		args[i] = va_arg(va, const char*);
		if (args[i] == NULL) {
			break;
		}
	}

	va_end(va);

	assert(i < 8);
	assert(args[i] == NULL);

	if ((proc_tmp = mb_process_start(filepath, args,
		MB_PROCESS_SUPERUSER | MB_PROCESS_WAIT,
		filepath, NULL, NULL)) > 0) {
		mb_process_wait(proc_tmp, &ret_tmp);
	}

	return ret_tmp;
}


/**
 * Mount all volumes.
 */
static void
sysinit_mount()
{
	int ret;

	/* mount /proc */
	ret = sysinit_execargs("/bin/mount", "-t", "proc", "proc", "/proc", NULL);
	if (ret != 0) {
		LOG_PRINT_ERROR("Could not mount /proc!");
	}

	/* mount root filesystem as read-write
	 * TODO: We want to leave it ro and use a separate
	 * partition for /var */
	ret = sysinit_execargs("/bin/mount", "-oremount,rw", "/", NULL);
	if (ret != 0) {
		LOG_PRINT_ERROR("Could not mount / read-write!");
	}

	/* create /dev/pts */
	if (mkdir_p("/dev/pts", S_IRWXU) == -1) {
		LOG_PRINT_ERROR("Could not create /dev/pts!");
	}

	/* create /dev/shm */
	if (mkdir_p("/dev/shm", S_IRWXU) == -1) {
		LOG_PRINT_ERROR("Could not create /dev/shm!");
	}

	/* process /etc/fstab */
	ret = sysinit_execargs("/bin/mount", "-a", NULL);
	if (ret != 0) {
		LOG_PRINT_ERROR("Could not mount all volumens (mount -a failed)!");
	}
}


/**
 * Initialize the logger
 */
static void
sysinit_logger(const char * const filepath)
{
	/* initialize the logging system */
	if (filepath == NULL) {
		log_setfile(stderr);
		exit(EXIT_FAILURE);
	} else {
		FILE *f;
		if ((f = fopen(filepath, "a")) == NULL) {
			fprintf(stderr, "main: Could not open logfile %s: %s\n",
				filepath, strerror(errno));
			exit(EXIT_FAILURE);
		}
		log_setfile(f);
	}
}


/**
 * Set the system hostname.
 */
static void
sysinit_hostname()
{
#ifdef LINUX
	int fd;
	size_t n = 0, tmp;
	FILE *f;
	char *hostname = NULL;

	if ((f = fopen("/etc/hostname", "r")) == NULL) {
		LOG_VPRINT_ERROR("Could not open /etc/hostname: %s!",
			strerror(errno));
		return;
	}

	if (getline(&hostname, &n, f) == -1) {
		LOG_VPRINT_ERROR("Could not read /etc/hostname: %s!",
			strerror(errno));
		fclose(f);
		return;
	}

	assert(hostname != NULL);

	/* make sure hostname doesn't end in new line */
	tmp = strlen(hostname) - 1;
	while (hostname[tmp] == '\n') {
		hostname[tmp] = '\0';
	}

	DEBUG_VPRINT("sysinit", "Setting hostname to %s", hostname);
	fclose(f);

	if ((fd = open("/proc/sys/kernel/hostname", O_WRONLY)) == -1) {
		LOG_VPRINT_ERROR("Could not open /proc/sys/kernel/hostname: %s",
			strerror(errno));
		free(hostname);
		return;
	}

	if (write(fd, hostname, strlen(hostname) + 1) == -1) {
		LOG_VPRINT_ERROR("Could not write to /proc/sys/kernel/hostname: %s",
			strerror(errno));
		/* best to keep that here or we may easily forget */
		goto end;
	}

end:
	free(hostname);
	close(fd);
#else
	int ret;
	ret = sysinit_execargs("/bin/hostname", "-F", "/etc/hostname", NULL);
	if (ret != 0) {
		LOG_PRINT_ERROR("Could not set system hostname!");
	}
#endif
}


/**
 * Seed /dev/random.
 */
static void
sysinit_random()
{
	int c, furand, fseed;
	char buf[1024];

	if ((fseed = open("/etc/random-seed", 0)) == -1) {
		LOG_PRINT_ERROR("Could not open /etc/random-seed!");
		return;
	}
	if ((furand = open("/dev/urandom", O_WRONLY)) == -1) {
		LOG_VPRINT_ERROR("Could not open /dev/urandom: %s",
			strerror(errno));
		close(fseed);
		return;
	}

	while ((c = read(fseed, buf, 1024 * sizeof(char))) > 0) {
		if (write(furand, buf, c) == -1) {
			LOG_VPRINT_ERROR("Could not write to /dev/urandom: %s!",
				strerror(errno));
			break;
		}
	}

	close(furand);
	close(fseed);
}


/**
 * Initialize network interfaces.
 */
static void
sysinit_network()
{
	int proc_tmp, ret_tmp = 0;

	const char * ifup_args[] =
	{
		"ifup",
		"-a",
		NULL
	};
	const char * ifconfig_eth0_args[] =
	{
		"ifconfig",
		"eth0",
		"10.0.2.15",
		NULL
	};
	const char * ifconfig_lo_args[] =
	{
		"ifconfig",
		"lo",
		"up",
		NULL
	};


	if ((proc_tmp = mb_process_start("/sbin/ifup", ifup_args,
		MB_PROCESS_SUPERUSER | MB_PROCESS_WAIT,
		"ifup", NULL, NULL)) > 0) {
		mb_process_wait(proc_tmp, &ret_tmp);
		if (ret_tmp != 0) {
			LOG_VPRINT_ERROR("ifup returned %i", ret_tmp);
		}
	}
	if ((proc_tmp = mb_process_start("/sbin/ifconfig", ifconfig_eth0_args,
		MB_PROCESS_SUPERUSER | MB_PROCESS_WAIT,
		"ifconfig_eth0", NULL, NULL)) > 0) {
		mb_process_wait(proc_tmp, &ret_tmp);
		if (ret_tmp != 0) {
			LOG_VPRINT_ERROR("ifconfig eth0 10.0.2.15 returned %i", ret_tmp);
		}
	}
	if ((proc_tmp = mb_process_start("/sbin/ifconfig", ifconfig_lo_args,
		MB_PROCESS_SUPERUSER | MB_PROCESS_WAIT,
		"ifconfig_lo", NULL, NULL)) > 0) {
		mb_process_wait(proc_tmp, &ret_tmp);
		if (ret_tmp != 0) {
			LOG_VPRINT_ERROR("ifconfig lo up returned %i", ret_tmp);
		}
	}
}


/**
 * Initialize dbus.
 */
static void
sysinit_dbus()
{
	int ret, proc_uuidgen;
	const char * uuidgen_args[] =
	{
		"dbus-uuidgen",
		"--ensure",
		NULL
	};

	const char * dbus_args[] =
	{
		"dbus-daemon",
		"--system",
		NULL
	};

	/* create runtime directories */
	if (mkdir_p("/var/lib/dbus", S_IRWXU) != 0) {
		LOG_PRINT_ERROR("Could not create directory /var/lib/dbus");
	}
	if (mkdir_p("/var/run/dbus", S_IRWXU) != 0) {
		LOG_PRINT_ERROR("Could not create directory /var/run/dbus");
	}

	/* ensure that a machine id has been generated */
	if ((proc_uuidgen = mb_process_start("/bin/dbus-uuidgen", uuidgen_args,
		MB_PROCESS_SUPERUSER | MB_PROCESS_WAIT, "dbus-uuidgen", NULL, NULL)) > 0) {
		mb_process_wait(proc_uuidgen, &ret);
	}

	/* start the dbus daemon */
	if ((proc_dbus = mb_process_start("/bin/dbus-daemon", dbus_args,
		MB_PROCESS_AUTORESTART | MB_PROCESS_NICE | MB_PROCESS_SUPERUSER,
		"dropbear", NULL, NULL)) == -1) {
		LOG_PRINT_ERROR("Could not start dropbear deamon!");
	}
}


/**
 * Start the dropbear daemon
 */
static void
sysinit_dropbear()
{
	const char * args[] =
	{
		"dropbear",
		"-R",
		NULL
	};

	if ((proc_dropbear = mb_process_start("/sbin/dropbear", args,
		MB_PROCESS_AUTORESTART | MB_PROCESS_NICE | MB_PROCESS_SUPERUSER,
		"dropbear", NULL, NULL)) == -1) {
		LOG_PRINT_ERROR("Could not start dropbear deamon!");
	}
}


/**
 * Launch tty on the console
 */
static void
sysinit_console()
{
	const char * args[] =
	{
		"getty",
		"-L",
		"console",
		"0",
		"vt100",
		NULL
	};

	if ((proc_getty = mb_process_start("/sbin/getty", args,
		MB_PROCESS_AUTORESTART | MB_PROCESS_SUPERUSER |
		MB_PROCESS_STDOUT_LOG | MB_PROCESS_STDERR_LOG,
		"getty", NULL, NULL)) == -1) {
		LOG_PRINT_ERROR("Could not start getty program!");
	}
}


int
sysinit_init(const char * const logfile)
{
	sysinit_mount();
	sysinit_logger(logfile);
	sysinit_random();
	sysinit_hostname();
	sysinit_dbus();
	sysinit_network();
	sysinit_dropbear();
	sysinit_console();
	return 0;
}


void
sysinit_shutdown(void)
{
	return;
}
