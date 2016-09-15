#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <limits.h>


#include "debug.h"
#include "log.h"
#include "linkedlist.h"
#include "timers.h"
#include "su.h"
#include "process.h"

#ifdef ENABLE_IONICE
#include "ionice.h"
#endif


#define MAX(a, b) ((a > b) ? a : b)


LISTABLE_STRUCT(mb_process,
	int id;
	pid_t pid;
	int stdin;
	int stdout;
	int stderr;
	enum mb_process_flags flags;
	const char *name;
	const char *binary;
	char * const * args;
	mb_process_exit exit_callback;
	int stopping;
);


LIST_DECLARE_STATIC(process_list);
static pthread_mutex_t process_list_lock = PTHREAD_MUTEX_INITIALIZER;

static int nextid = 1;
static pthread_t monitor_thread;
static pthread_t io_thread;
static int quit = 0;


/**
 * mb_process_checkflagsoneof() -- Checks that only one of the specified
 * flags is set
 */
static int
mb_process_checkflagsoneof(enum mb_process_flags proc_flags, enum mb_process_flags flags)
{
	int i;
	if (proc_flags & flags) {
		proc_flags &= flags;
		for (i = 0; i < sizeof(enum mb_process_flags) * CHAR_BIT; i++) {
			if ((proc_flags & 1) && proc_flags != 1) {
				errno = EINVAL;
				return -1;
			}
			proc_flags >>= 1;
		}
	}
	return 0;
}


/**
 * mb_process_getbyid() -- Gets a process from the list by it's id.
 */
static struct mb_process*
mb_process_getbyid(int id)
{
	struct mb_process *proc, *ret = NULL;

	pthread_mutex_lock(&process_list_lock);

	LIST_FOREACH(struct mb_process*, proc, &process_list) {
		if (proc->id == id) {
			ret = proc;
			break;
		}
	}

	pthread_mutex_unlock(&process_list_lock);

	return ret;
}


/**
 * mb_process_fork() -- Forks and execs a process.
 */
static pid_t
mb_process_fork(struct mb_process *proc)
{
	int ret = -1;
	int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 };

	if (pipe(in) == -1 || pipe(out) == -1 || pipe(err) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "process", "pipe() failed");
		goto end;
	}

	if ((proc->pid = fork()) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "process", "fork() failed");
		goto end;

	} else if (proc->pid != 0) {
		/* close child end of pipes */
		close(in[0]);
		close(out[1]);
		close(err[1]);

		proc->stdin = in[1];
		proc->stdout = out[0];
		proc->stderr = err[0];

		/* fork() succeeded so return the pid of the new process */
		return proc->pid;
	}

	/**** Child process falls through ****/

	/* close parent end of pipes */
	close(in[1]);
	close(out[0]);
	close(err[0]);

	/* duplicate standard file descriptors */
	if (dup2(in[0], STDIN_FILENO) == -1 ||
		dup2(out[1], STDOUT_FILENO) == -1 ||
		dup2(err[1], STDERR_FILENO) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "process", "dup2() failed");
		exit(EXIT_FAILURE);
	}

	/* set the process niceness */
	if (proc->flags & MB_PROCESS_NICE) {
		if (nice(5) == -1) {
			LOG_VPRINT(MB_LOGLEVEL_WARN, "process",
				"I'm trying to be nice but I can't. (errno=%i)", errno);
		}
	}

#ifdef ENABLE_IONICE
	/* set the process IO priority */
	if (proc->flags & MB_PROCESS_IONICE_IDLE) {
		(void) mb_su_gainroot();
		if (ioprio_set(IOPRIO_WHO_PROCESS, getpid(), IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0)) == -1) {
			fprintf(stderr, "process: WARNING: Could not set deluged IO priority to idle!!\n");
		}
		(void) mb_su_droproot();
	} else if (proc->flags & MB_PROCESS_IONICE_BE) {
		(void) mb_su_gainroot();
		if (ioprio_set(IOPRIO_WHO_PROCESS, getpid(), IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 0)) == -1) {
			fprintf(stderr, "process: WARNING: Could not set deluged IO priority to idle!!\n");
		}
		(void) mb_su_droproot();
	}
#endif

	/* if the process requires root then elevate privilege */
	if (proc->flags & MB_PROCESS_SUPERUSER) {
		mb_su_gainroot();
	}

	/* execute the process */
	execv(proc->binary, proc->args);

	/* if execv() failed then exit with error status */
	exit(EXIT_FAILURE);

end:
	if (in[0] != -1) close(in[0]);
	if (in[1] != -1) close(in[1]);
	if (out[0] != -1) close(out[0]);
	if (out[1] != -1) close(out[1]);
	if (err[0] != -1) close(err[0]);
	if (err[1] != -1) close(err[1]);
	return ret;
}


/**
 * mb_process_clone_args() -- Clones a list of arguments.
 */
static char * const*
mb_process_clone_args(char * const argv[])
{
	char **args;
	int sz = 0, i = 0;

	/* walk the arguments list and calculate it's size */
	while (argv[i++] != NULL) {
		sz++;
	}

	/* allocate memory for the pointer array */
	if ((args = malloc((sz + 1) * sizeof(char *))) == NULL) {
		DEBUG_PRINT("process", "Cannot clone args. Out of memory");
		return NULL;
	}

	/* duplicate all the entries and populate the pointer array
	 * with pointers. If any of the strdup() fails free everything
	 * we've allocated so far and return NULL */
	for (i = 0; i < sz; i++) {
		if ((args[i] = strdup(argv[i])) == NULL) {
			DEBUG_PRINT("process", "Cannot clone args. Out of memory.");
			sz = i;
			for (i = 0; i < sz; i++) {
				free(args[i]);
			}
			free(args);
			return NULL;
		}
	}

	/* set the NULL terminator entry */
	args[i] = NULL;

	return args;
}


/**
 * mb_process_free_args() -- Free's a cloned list of arguments.
 */
static void
mb_process_free_args(char * const argv[]) {
	int i = 0;
	while (argv[i] != NULL) {
		free(argv[i++]);
	}
	free((void*) argv);
}


/**
 * mb_process_free() -- Free a process structure.
 */
static void
mb_process_free(struct mb_process *proc)
{
	assert(proc != NULL);

	if (proc->name != NULL) {
		free((void*) proc->name);
	}
	if (proc->binary != NULL) {
		free((void*) proc->binary);
	}
	if (proc->args != NULL) {
		mb_process_free_args(proc->args);
	}
	free(proc);
}


/**
 * mb_process_get_next_id() -- Gets the next process id.
 */
static inline int
mb_process_get_next_id(void)
{
	return nextid++;
}


/**
 * mb_process_force_kill() -- This is the handler a timer that is set when
 * mb_process_stop() is called and the process does not have the
 * MB_PROCESS_SIGKILL flag set. It fires 5 seconds after stop() returns
 * and will SIGKILL the process (in case SIGTERM didn't work).
 */
static enum mbt_result
mb_process_force_kill(int id, void *data)
{
	int proc_id = *((int*) data);
	struct mb_process *proc;

	pthread_mutex_lock(&process_list_lock);

	LIST_FOREACH(struct mb_process*, proc, &process_list) {
		if (proc->id == proc_id) {
			if (kill(proc->pid, SIGKILL) == -1) {
				LOG_PRINT(MB_LOGLEVEL_ERROR, "process", "kill() regurned -1");
			}

			pthread_mutex_unlock(&process_list_lock);

			return MB_TIMER_CALLBACK_RESULT_CONTINUE;
		}
	}

	pthread_mutex_unlock(&process_list_lock);

	free(data);

	return MB_TIMER_CALLBACK_RESULT_STOP;
}


/**
 * mb_process_io_thread() -- Runs on it's own thread and handles standard IO
 * to/from processes.
 */
static void *
mb_process_io_thread(void *arg)
{
	fd_set fds;
	int fd_max, res;
	char buf[1024];
	struct mb_process *proc;
	struct timeval tv;

	while (!quit) {

		fd_max = 0;
		FD_ZERO(&fds);

		/* build a file descriptor set to select() */
		pthread_mutex_lock(&process_list_lock);
		LIST_FOREACH(struct mb_process*, proc, &process_list) {
			if (proc->stdout != -1) {
				FD_SET(proc->stdout, &fds);
				fd_max = MAX(fd_max, proc->stdout);
			}
			if (proc->stderr != -1) {
				FD_SET(proc->stderr, &fds);
				fd_max = MAX(fd_max, proc->stderr);
			}
		}
		pthread_mutex_unlock(&process_list_lock);

		/* select the output file descriptors of all processes */
		tv.tv_sec = 0;
		tv.tv_usec = 500 * 1000L;
		if ((res = select(fd_max + 1, &fds, NULL, NULL, &tv)) == 0) {
			continue;
		} else if (res < 0) {
			if (errno == EINTR) {
				continue;
			}
			LOG_VPRINT(MB_LOGLEVEL_ERROR, "process", "select() returned -1 (errno=%i)",
				errno);
			usleep(500 * 1000L * 1000L);
			continue;
		}

		/* process all pending output */
		pthread_mutex_lock(&process_list_lock);
		LIST_FOREACH(struct mb_process*, proc, &process_list) {
			if (proc->stdout != -1 && FD_ISSET(proc->stdout, &fds)) {
				if (!(proc->flags & MB_PROCESS_STDOUT_PIPE)) {
					if ((res = read(proc->stdout, buf, sizeof(buf))) == -1) {
						LOG_VPRINT(MB_LOGLEVEL_ERROR, "process",
							"read() returned -1 (errno=%i)", errno);
					}
					if (proc->flags & MB_PROCESS_STDOUT_LOG) {
						/* TODO: We need to break the output in lines */
						LOG_VPRINT(MB_LOGLEVEL_WARN, "process",
							"%s: TODO: Log STDOUT output!!", proc->name);
					}
				}
			}
			if (proc->stderr != -1 && FD_ISSET(proc->stderr, &fds)) {
				if (!(proc->flags & MB_PROCESS_STDERR_PIPE)) {
					if ((res = read(proc->stdout, buf, sizeof(buf))) == -1) {
						LOG_VPRINT(MB_LOGLEVEL_ERROR, "process",
							"read() returned -1 (errno=%i)", errno);
					}
					if (proc->flags & MB_PROCESS_STDERR_LOG) {
						/* TODO: We need to break the output in lines */
						LOG_VPRINT(MB_LOGLEVEL_WARN, "process",
							"%s: TODO: Log STDERR output", proc->name);
					}
				}
			}
		}
		pthread_mutex_unlock(&process_list_lock);

	}
	return NULL;
}


/**
 * mb_process_monitor_thread() -- This function runs on it's own thread and
 * waits for processes to exit and then handles the event appropriately.
 */
static void *
mb_process_monitor_thread(void *arg)
{
	pid_t pid;
	int status;
	struct mb_process *proc;

	MB_DEBUG_SET_THREAD_NAME("proc-mon");
	DEBUG_PRINT("process", "Starting process monitor thread");

	while (!quit || LIST_SIZE(&process_list) > 0) {
		if ((pid = wait(&status)) == -1) {
			if (errno == EINTR) {
				continue;
			} else if (errno == ECHILD) {
				usleep(500 * 1000L * 1000L);
				continue;
			}
			fprintf(stderr, "process: wait() returned -1 (errno=%i)\n",
				errno);
			break;
		}

		pthread_mutex_lock(&process_list_lock);

		LIST_FOREACH_SAFE(struct mb_process*, proc, &process_list, {
			if (proc->pid == pid) {

				/* close file descriptors */
				if (proc->stdin != -1) close(proc->stdin);
				if (proc->stdout != -1) close(proc->stdout);
				if (proc->stderr != -1) close(proc->stderr);

				/* clear file descriptors and PID */
				proc->pid = -1;
				proc->stdin = -1;
				proc->stdout = -1;
				proc->stderr = -1;

				/* if the process terminated abnormally then log
				 * an error message */
				if (WEXITSTATUS(status)) {
					LOG_VPRINT(MB_LOGLEVEL_WARN, "process",
						"Process '%s' exitted with status %i (id=%i,pid=%i)",
						proc->name, WEXITSTATUS(status), proc->id, pid);
				}

				/* if the process terminated abormally and the AUTORESTART flag is
				 * set then restart the process */
				if (WEXITSTATUS(status) != 0 && (proc->flags & MB_PROCESS_AUTORESTART)) {
					if (!proc->stopping) {
						LOG_VPRINT(MB_LOGLEVEL_ERROR, "process",
							"Auto restarting process '%s' (id=%i,pid=%i)",
							proc->name, proc->id, pid);
						proc->pid = mb_process_fork(proc);
						continue;
					}
				}

				/* remove process from list */
				LIST_REMOVE(proc);

				/* invoke the process exit callback */
				if (proc->exit_callback != NULL) {
					proc->exit_callback(proc->id,
						WEXITSTATUS(status));
				}

				/* cleanup */
				mb_process_free(proc);

				break;
			}
		});

		pthread_mutex_unlock(&process_list_lock);
	}
	return NULL;
}


/**
 * mb_process_getpid() -- Gets the pid of a process.
 *
 * NOTE: The PID cannot be used to identify a process using the process
 * API since a process may crash or get restarted and get a new PID.
 */
pid_t
mb_process_getpid(int id)
{
	struct mb_process *proc;

	if ((proc = mb_process_getbyid(id)) != NULL) {
		return proc->pid;
	}
	return -1;
}


/**
 * mb_process_openfd() -- Opens one of the standard file descriptors for
 * the process.
 *
 * NOTE: After opening a file descriptor with this function the process
 * manager stops managing the file descriptor so you must call close()
 * on it when you're done using it.
 */
int
mb_process_openfd(int id, int std_fileno)
{
	int result = -1;
	struct mb_process *proc;

	assert(std_fileno == STDIN_FILENO ||
		std_fileno == STDOUT_FILENO ||
		std_fileno == STDERR_FILENO);

	/* get the process object */
	if ((proc = mb_process_getbyid(id)) == NULL) {
		DEBUG_VPRINT("process", "Process id %i not found", id);
		errno = ENOENT;
		return -1;
	}

	/* Clear the file descriptor from the process object and
	 * return it */
	switch (std_fileno) {
	case STDIN_FILENO:
		result = proc->stdin;
		proc->stdin = -1;
		break;
	case STDOUT_FILENO:
		result = proc->stdout;
		proc->stdout = -1;
		break;
	case STDERR_FILENO:
		result = proc->stderr;
		proc->stderr = -1;
		break;
	default:
		result = -1;
	}
	return result;
}


/**
 * mb_process_start() -- Starts and monitors a child process.
 */
int
mb_process_start(const char *binary, char * const argv[],
	enum mb_process_flags flags, const char *name, mb_process_exit exit_callback)
{
	int ret = -1;
	struct mb_process *proc;

	assert(binary != NULL);
	assert(argv != NULL);
	assert(name != NULL);

	/* check for conflicting IO priority flags */
	if (mb_process_checkflagsoneof(flags, MB_PROCESS_IONICE) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "process",
			"Multiple IO priorities set!");
		return -1;
	}

	/* check for conflicting STDOUT flags */
	if (mb_process_checkflagsoneof(flags, MB_PROCESS_STDOUT) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR,  "process",
			"Multiple STDOUT flags set!");
		return -1;
	}

	/* check for conflicting STDERR flags */
	if (mb_process_checkflagsoneof(flags, MB_PROCESS_STDERR) == -1) {
		LOG_PRINT(MB_LOGLEVEL_ERROR,  "process",
			"Multiple STDERR flags set!");
		return -1;
	}

	/* allocate memory for process structure */
	if ((proc = malloc(sizeof(struct mb_process))) == NULL) {
		LOG_PRINT(MB_LOGLEVEL_ERROR,  "process", "Out of memory");
		return -1;
	}

	/* initialize process structure and add it to list */
	proc->id = mb_process_get_next_id();
	proc->stdin = -1;
	proc->stdout = -1;
	proc->stderr = -1;
	proc->stopping = 0;
	proc->flags = flags;
	proc->args = mb_process_clone_args(argv);
	proc->name = strdup(name);
	proc->binary = strdup(binary);
	proc->exit_callback = exit_callback;

	/* check that all memory allocations succeeded */
	if (proc->binary == NULL || proc->args == NULL || proc->name == NULL) {
		LOG_PRINT(MB_LOGLEVEL_ERROR,  "process", "Out of memory");
		mb_process_free(proc);
		return -1;
	}

	/* We need to lock the list BEFORE FORKING to ensure that even if
	 * the child dies right away the exit code is caught and processed */
	pthread_mutex_lock(&process_list_lock);
	if ((proc->pid = mb_process_fork(proc)) != -1) {
		LIST_ADD(&process_list, proc);
		ret = proc->id;
	}
	pthread_mutex_unlock(&process_list_lock);

	return ret;
}


/**
 * mb_process_stop() -- Stops a running child process.
 */
int
mb_process_stop(int id)
{
	struct mb_process *proc;

	if ((proc = mb_process_getbyid(id)) != NULL) {

		proc->stopping = 1;

		if (proc->flags & MB_PROCESS_SIGKILL) {
			/* send SIGKILL to the process */
			if (kill(proc->pid, SIGKILL) == -1) {
				/* TODO: Is this guaranteed to succeed?
				 * Should we abort() here? */
				LOG_VPRINT(MB_LOGLEVEL_ERROR, "process",
					"kill(pid, SIGKILL) returned -1 (errno=%i)", errno);
				return -1;
			}
		} else {
			struct timespec tv;
			int *id_copy;

			/* allocate heap memory for a copy of the id */
			if ((id_copy = malloc(sizeof(int))) == NULL) {
				LOG_PRINT(MB_LOGLEVEL_ERROR, "process",
					"Cannot kill service: Out of memory");
				return -1;
			}

			/* send SIGTERM to the process */
			if (kill(proc->pid, SIGTERM) == -1) {
				LOG_VPRINT(MB_LOGLEVEL_ERROR, "process",
					"kill(pid, SIGTERM) returned -1 (errno=%i)", errno);
				return -1;
			}

			/* register a timer to SIGKILL the process in 5 minutes if it
			 * hasn't exited by then */
			tv.tv_sec = 5;
			tv.tv_nsec = 0;
			*id_copy = id;
			if (mbt_register(&tv, MB_TIMER_TYPE_AUTORELOAD, mb_process_force_kill, id_copy) == -1) {
				LOG_PRINT(MB_LOGLEVEL_ERROR, "process",
					"Could not register force stop timer");
				free(id_copy);
				return -1;
			}
		}
		return 0;

	} else {
		errno = ENOENT;
		return -1;
	}
}


/**
 * mb_process_init() -- Initialize the child process monitor
 */
int
mb_process_init(void)
{
	DEBUG_PRINT("process", "Initializing process monitor");

	LIST_INIT(&process_list);

	if (pthread_create(&monitor_thread, NULL, mb_process_monitor_thread, NULL) != 0) {
		fprintf(stderr, "process: Could not start thread\n");
		return -1;
	}

	if (pthread_create(&io_thread, NULL, mb_process_io_thread, NULL) != 0) {
		fprintf(stderr, "process: Could not start IO thread\n");
		quit = 1;
		pthread_join(io_thread, 0);
		return -1;
	}

	return 0;
}


/**
 * mb_process_shutdown() -- Shutdown the process monitor.
 */
void
mb_process_shutdown(void)
{
	struct mb_process *proc;

	DEBUG_PRINT("process", "Shutting down process monitor");

	LIST_FOREACH_SAFE(struct mb_process*, proc, &process_list, {
		mb_process_stop(proc->id);
	})

	quit = 1;
	pthread_join(monitor_thread, 0);
	pthread_join(io_thread, 0);
}
