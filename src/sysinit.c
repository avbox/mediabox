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
#include "settings.h"


#define UDEVD_BIN	"/sbin/udevd"
#define UDEVADM_BIN	"/sbin/udevadm"


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

	if ((proc_tmp = avbox_process_start(filepath, args,
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT,
		filepath, NULL, NULL)) > 0) {
		avbox_process_wait(proc_tmp, &ret_tmp);
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
	int fd = -1;
	char *hostname = NULL;

	/* get the hostname from database */
	if ((hostname = avbox_settings_getstring("hostname")) == NULL) {
		LOG_VPRINT_ERROR("Could not get hostname setting: %s",
			strerror(errno));
		return;
	}

	DEBUG_VPRINT("sysinit", "Setting hostname to %s", hostname);

	/* configure hostname */
	if ((fd = open("/proc/sys/kernel/hostname", O_WRONLY)) == -1) {
		LOG_VPRINT_ERROR("Could not open /proc/sys/kernel/hostname: %s",
			strerror(errno));
		goto end;
	}
	if (write(fd, hostname, strlen(hostname) + 1) == -1) {
		LOG_VPRINT_ERROR("Could not write to /proc/sys/kernel/hostname: %s",
			strerror(errno));
		/* best to keep that here or we may easily forget */
		goto end;
	}

end:
	if (hostname != NULL) {
		free(hostname);
	}
	if (fd != -1) {
		close(fd);
	}
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


static void
sysinit_udevd()
{
	int proc_tmp, ret_tmp = 0;

	const char * udevd_args[] =
	{
		"udevd",
		"-d",
		NULL
	};


	if ((proc_tmp = avbox_process_start(UDEVD_BIN, udevd_args,
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT,
		"udevd", NULL, NULL)) > 0) {
		avbox_process_wait(proc_tmp, &ret_tmp);
		if (ret_tmp != 0) {
			LOG_VPRINT_ERROR("ifup returned %i", ret_tmp);
			return;
		}
	}

	if ((ret_tmp = sysinit_execargs(UDEVADM_BIN, "trigger", "--type=subsystems",
		"--action=add", NULL))) {
		LOG_VPRINT_ERROR("`%s trigger --type=subsystems --action=add` returned %i",
			UDEVADM_BIN, ret_tmp);
	}
	if ((ret_tmp = sysinit_execargs(UDEVADM_BIN, "trigger", "--type=devices",
		"--action=add", NULL))) {
		LOG_VPRINT_ERROR("`%s trigger --type=devices --action=add` returned %i",
			UDEVADM_BIN, ret_tmp);
	}
	if ((ret_tmp = sysinit_execargs(UDEVADM_BIN, "settle", "--timeout=30", NULL))) {
		LOG_VPRINT_ERROR("`%s settle --timeout=30` returned %i",
			UDEVADM_BIN, ret_tmp);
	}

	return;
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
	const char * ifconfig_lo_args[] =
	{
		"ifconfig",
		"lo",
		"up",
		NULL
	};


	if ((proc_tmp = avbox_process_start("/sbin/ifup", ifup_args,
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT,
		"ifup", NULL, NULL)) > 0) {
		avbox_process_wait(proc_tmp, &ret_tmp);
		if (ret_tmp != 0) {
			LOG_VPRINT_ERROR("ifup returned %i", ret_tmp);
		}
	}
	if ((proc_tmp = avbox_process_start("/sbin/ifconfig", ifconfig_lo_args,
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT,
		"ifconfig_lo", NULL, NULL)) > 0) {
		avbox_process_wait(proc_tmp, &ret_tmp);
		if (ret_tmp != 0) {
			LOG_VPRINT_ERROR("ifconfig lo up returned %i", ret_tmp);
		}
	}
#if 1
	const char * udhcpc_eth0_args[] =
	{
		"udhcpc",
		"-i",
		"eth0",
		"-n",
		NULL
	};

	if ((proc_tmp = avbox_process_start("/sbin/udhcpc", udhcpc_eth0_args,
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT,
		"udhcpc_eth0", NULL, NULL)) > 0) {
		avbox_process_wait(proc_tmp, &ret_tmp);
		if (ret_tmp != 0) {
			LOG_VPRINT_ERROR("`udhcpc -i eth0 -n` returned %i", ret_tmp);
		}
	}

#else
	const char * ifconfig_eth0_args[] =
	{
		"ifconfig",
		"eth0",
		"10.0.2.15",
		NULL
	};

	if ((proc_tmp = avbox_process_start("/sbin/ifconfig", ifconfig_eth0_args,
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT,
		"ifconfig_eth0", NULL, NULL)) > 0) {
		avbox_process_wait(proc_tmp, &ret_tmp);
		if (ret_tmp != 0) {
			LOG_VPRINT_ERROR("ifconfig eth0 10.0.2.15 returned %i", ret_tmp);
		}
	}
#endif
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
	if ((proc_uuidgen = avbox_process_start("/bin/dbus-uuidgen", uuidgen_args,
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT, "dbus-uuidgen", NULL, NULL)) > 0) {
		avbox_process_wait(proc_uuidgen, &ret);
	}

	/* start the dbus daemon */
	if ((proc_dbus = avbox_process_start("/bin/dbus-daemon", dbus_args,
		AVBOX_PROCESS_AUTORESTART | AVBOX_PROCESS_NICE | AVBOX_PROCESS_SUPERUSER,
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

	if ((proc_dropbear = avbox_process_start("/sbin/dropbear", args,
		AVBOX_PROCESS_AUTORESTART | AVBOX_PROCESS_NICE | AVBOX_PROCESS_SUPERUSER,
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
		"-n",
		"-l",
		"/bin/sh",
		"console",
		"0",
		"vt100",
		NULL
	};

	if ((proc_getty = avbox_process_start("/sbin/getty", args,
		AVBOX_PROCESS_AUTORESTART_ALWAYS | AVBOX_PROCESS_SUPERUSER,
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
	sysinit_udevd();
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
	sysinit_execargs(UDEVADM_BIN, "control", "--stop-exec-queue", NULL);
	/* killall udevd */
	return;
}
