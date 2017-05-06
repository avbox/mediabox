#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

#define LOG_MODULE "application"

#include "ui/video.h"
#include "ui/input.h"
#include "audio.h"
#include "log.h"
#include "debug.h"
#include "dispatch.h"
#include "thread.h"
#include "application.h"
#include "settings.h"
#include "timers.h"
#include "process.h"
#include "sysinit.h"
#include "su.h"
#ifdef ENABLE_IONICE
#include "ionice.h"
#endif


static int quit = 0;
static int pid1 = 0;
static int result = 0;
static struct avbox_dispatch_object *dispatch_object;


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
		LOG_PRINT_INFO("Received SIGTERM");
		avbox_application_quit(0);
		break;
	default:
		DEBUG_VPRINT("main", "Received signal: %i", signum);
		break;
	}
}


/**
 * Parse kernel arguments
 */
static char **
parse_kernel_args(const char * const prog, int *argc)
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
		LOG_VPRINT_ERROR("Could not mount proc: %s",
			strerror(errno));
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
						LOG_VPRINT_ERROR("%s: Too many arguments! This build "
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
 * Delegate a function call to the application's thread.
 */
struct avbox_delegate*
avbox_application_delegate(avbox_delegate_func func, void *arg)
{
	struct avbox_delegate *del;

	/* create delegate */
	if ((del = avbox_delegate_new(func, arg)) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

	/* send delegate to the main thread */
	if (avbox_dispatch_sendmsg(-1, &dispatch_object,
		AVBOX_MESSAGETYPE_DELEGATE, AVBOX_DISPATCH_UNICAST, del) == NULL) {
	}

	return del;
}


/**
 * Receive application messages.
 */
static int
avbox_application_msghandler(void *context, struct avbox_message *msg)
{
	switch (avbox_dispatch_getmsgtype(msg)) {
	case AVBOX_MESSAGETYPE_INPUT:
	{
		struct avbox_input_message * const ev =
			avbox_dispatch_getmsgpayload(msg);
		avbox_input_eventfree(ev);
		break;
	}
	case AVBOX_MESSAGETYPE_DELEGATE:
	{
		struct avbox_delegate * const del =
			avbox_dispatch_getmsgpayload(msg);
		avbox_delegate_execute(del);
		break;
	}
	case AVBOX_MESSAGETYPE_QUIT:
	{
		quit = 1;
		break;
	}
	default:
		DEBUG_ABORT("application", "Received invalid message!");
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Initialize application.
 */
int
avbox_application_init(int argc, char **cargv, const char *logf)
{
	int i;
	char **argv;
	const char * logfile = NULL;

	pid1 = (getpid() == 1);

	/* initialize logging system for early
	 * logging */
	log_init();

	/* if we're running as pid 1 parse arguments from
	 * kernel */
	if (pid1) {
		if ((argv = parse_kernel_args(cargv[0], &argc)) == NULL) {
			LOG_VPRINT_ERROR("%s: Cannot parse kernel args: %s (%i)",
				cargv[0], strerror(errno), errno);
			exit(EXIT_FAILURE);
		}
	} else {
		argv = cargv;
	}

	if (logf != NULL) {
		logfile = logf;
	}

	/* parse command line */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--avbox:init")) {
			pid1 = 1;
		} else if (!strcmp(argv[i], "--avbox:logfile")) {
			logfile = argv[++i];
		}
	}

	/* if no logfile was specified and we're running
	 * as init then use the default */
	if (pid1 && logfile == NULL) {
		logfile = "/var/log/avbox.log";
	}

	/* initialize message dispatcher */
	if (avbox_dispatch_init() == -1) {
		LOG_PRINT_ERROR("Could not initialize dispatcher");
		return -1;
	}

	/* initializing thread pool */
	if (avbox_thread_init() == -1) {
		LOG_PRINT_ERROR("Could not initialize thread pool");
		return -1;
	}

	/* initialize settings database */
	if (avbox_settings_init() == -1) {
		LOG_PRINT_ERROR("Could not initialize settings database");
		return -1;
	}

	/* initialize timers system */
	if (avbox_timers_init() != 0) {
		LOG_PRINT_ERROR("Could not initialize timers subsystem");
		return -1;
	}

	/* initialize process manager */
	if (avbox_process_init() != 0) {
		LOG_PRINT_ERROR("Could not initialize timers subsystem");
		return -1;
	}

	/* initialize video device */
	if (avbox_video_init(argc, argv) == -1) {
		LOG_PRINT_ERROR("Could not initialize video subsystem");
		return -1;
	}

	/* initialize system */
	if (pid1 && sysinit_init(logfile) != 0) {
		LOG_PRINT_ERROR("Could not initialize system");
		return -1;
	} else if (!pid1 && logfile != NULL) {
		FILE *f;
		if ((f = fopen(logfile, "a")) == NULL) {
			LOG_VPRINT_ERROR("Could not open logfile %s: %s\n",
				logfile, strerror(errno));
			exit(EXIT_FAILURE);
		}
		log_setfile(f);
	}

	/* initialize input system */
	if (avbox_input_init(argc, argv) != 0) {
		LOG_PRINT_ERROR("Could not initialize input subsystem");
		return -1;
	}

	/* initialize audio subsystem */
	if (avbox_audiostream_init() != 0) {
		LOG_PRINT_ERROR("Could not initialize audio subsystem");
	}

#ifdef ENABLE_IONICE
	if (ioprio_set(IOPRIO_WHO_PROCESS, getpid(), IOPRIO_PRIO_VALUE(IOPRIO_CLASS_RT, 4)) == -1) {
		LOG_PRINT_ERROR("Could not set priority to realtime");
	}
#endif

	/* if we're running as pid 1 free the list
	 * of kernel arguments */
	if (pid1) {
		free_kernel_args(argc, argv);
	}


	/* drop root prividges after initializing framebuffer */
	avbox_droproot();
	return 0;
}


/**
 * Application main loop.
 */
int
avbox_application_run(void)
{
	DEBUG_PRINT("application", "Running application");

	/* create dispatch object */
	if ((dispatch_object = avbox_dispatch_createobject(
		avbox_application_msghandler, 0, NULL)) == NULL) {
		LOG_VPRINT_ERROR("Could not create dispatch object: %s",
			strerror(errno));
		return -1;
	}

	/* install signal handlers */
	if (signal(SIGTERM, signal_handler) == SIG_ERR ||
		signal(SIGHUP, signal_handler) == SIG_ERR ||
		signal(SIGINT, signal_handler) == SIG_ERR) {
		LOG_PRINT_ERROR("Could not set signal handlers");
		avbox_dispatch_destroyobject(dispatch_object);
		dispatch_object = NULL;
		return -1;
	}

	/* run the message loop */
	while (!quit) {
		struct avbox_message *msg;
		/* get the next message */
		if ((msg = avbox_dispatch_getmsg()) == NULL) {
			switch (errno) {
			case EAGAIN: continue;
			default:
				DEBUG_VABORT("application", "Unexpected error: %s (%i)",
					strerror(errno), errno);
			}
		}
		avbox_dispatch_dispatchmsg(msg);
	}

	DEBUG_PRINT("application", "Application quitting");

	/* unintall signal handlers */
	if (signal(SIGTERM, SIG_DFL) == SIG_ERR ||
		signal(SIGHUP, SIG_DFL) == SIG_ERR ||
		signal(SIGINT, SIG_DFL) == SIG_ERR) {
		LOG_PRINT_ERROR("Could not uninstall signal handlers");
	}

	/* destroy dispatch object */
	avbox_dispatch_destroyobject(dispatch_object);
	dispatch_object = NULL;

	/* cleanup */
	avbox_audiostream_shutdown();
	avbox_process_shutdown();
	avbox_timers_shutdown();
	avbox_settings_shutdown();
	avbox_input_shutdown();
	avbox_thread_shutdown();
	avbox_dispatch_shutdown();
	avbox_video_shutdown();

	DEBUG_VPRINT("application", "Exiting (status=%i)",
		result);

	/* if we're running as pid 1 then reboot */
	if (pid1) {
		DEBUG_PRINT("application", "Rebooting");
		if (reboot(LINUX_REBOOT_CMD_RESTART) == -1) {
			abort();
		}
	}

	return result;
}


/**
 * Dispatch the next message in the thread's
 * queue.
 */
int
avbox_application_doevents()
{
	struct avbox_message *msg;
	/* get the next message */
	if (avbox_dispatch_peekmsg() != NULL) {
		if ((msg = avbox_dispatch_getmsg()) == NULL) {
			DEBUG_ABORT("application",
				"BUG: getmsg() returned NULL after peekmsg() succeeded!");
		}
		avbox_dispatch_dispatchmsg(msg);
		return 1;
	}
	return 0;
}

/**
 * Quit the application.
 */
int
avbox_application_quit(int status)
{
	result = status;
	if (avbox_dispatch_sendmsg(-1, &dispatch_object, AVBOX_MESSAGETYPE_QUIT,
		AVBOX_DISPATCH_UNICAST, NULL) == NULL) {
		LOG_VPRINT_ERROR("Could not send QUIT message: %s",
			strerror(errno));
		return -1;
	}
	return 0;
}
