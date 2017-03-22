/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

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

#define LOG_MODULE "process"

#include "debug.h"
#include "log.h"
#include "linkedlist.h"
#include "timers.h"
#include "su.h"
#include "process.h"
#include "math_util.h"
#include "file_util.h"


#ifdef ENABLE_IONICE
#include "ionice.h"
#endif


LISTABLE_STRUCT(mb_process,
	int id;
	pid_t pid;
	int stdin;
	int stdout;
	int stderr;
	int exit_status;
	int exitted;
	unsigned force_kill_delay;
	unsigned autorestart_delay;
	enum mb_process_flags flags;
	const char *name;
	const char *binary;
	char * const * args;
	mb_process_exit exit_callback;
	void *exit_callback_data;
	int stopping;
	pthread_cond_t cond;
);


LIST_DECLARE_STATIC(process_list);
static pthread_mutex_t process_list_lock = PTHREAD_MUTEX_INITIALIZER;

static int nextid = 1;
static pthread_t monitor_thread;
static pthread_t io_thread;
static int quit = 0, io_quit = 0;


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
mb_process_getbyid(const int id, const int locked)
{
	struct mb_process *proc, *ret = NULL;

	if (!locked) {
		pthread_mutex_lock(&process_list_lock);
	}

	LIST_FOREACH(struct mb_process*, proc, &process_list) {
		if (proc->id == id) {
			ret = proc;
			break;
		}
	}

	if (!locked) {
		pthread_mutex_unlock(&process_list_lock);
	}

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

	/* close all file descriptors >= 3 */
	closefrom(3);

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
mb_process_clone_args(const char * const argv[])
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
	enum mbt_result ret = MB_TIMER_CALLBACK_RESULT_STOP;

	DEBUG_VPRINT("process", "Force kill callback for process %id",
		proc_id);

	pthread_mutex_lock(&process_list_lock);

	LIST_FOREACH(struct mb_process*, proc, &process_list) {
		if (proc->id == proc_id) {
			DEBUG_VPRINT("process", "Force killing process %i (pid=%i)",
				proc_id, proc->pid);
			if (kill(proc->pid, SIGKILL) == -1) {
				LOG_PRINT_ERROR("kill() regurned -1");
			}
			ret = MB_TIMER_CALLBACK_RESULT_CONTINUE;
			break;
		}
	}

	pthread_mutex_unlock(&process_list_lock);

	free(data);

	return ret;
}


/**
 * Restarts a process that is not currently running.
 * This is only called after a process chrashes.
 */
static enum mbt_result
mb_process_autorestart(int id, void *data)
{
	struct mb_process * const proc = (struct mb_process * const) data;
	proc->pid = mb_process_fork(proc);
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

	MB_DEBUG_SET_THREAD_NAME("proc-io");
	DEBUG_PRINT("process", "Starting IO thread");

	while (!io_quit) {

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
							"%s: %s", proc->name, buf);
					}
				}
			}
			if (proc->stderr != -1 && FD_ISSET(proc->stderr, &fds)) {
				if (!(proc->flags & MB_PROCESS_STDERR_PIPE)) {
					if ((res = read(proc->stderr, buf, sizeof(buf))) == -1) {
						LOG_VPRINT(MB_LOGLEVEL_ERROR, "process",
							"read() returned -1 (errno=%i)", errno);
					}
					if (proc->flags & MB_PROCESS_STDERR_LOG) {
						/* TODO: We need to break the output in lines */
						LOG_VPRINT(MB_LOGLEVEL_WARN, "process",
							"%s: %s", proc->name, buf);
					}
				}
			}
		}
		pthread_mutex_unlock(&process_list_lock);

	}

	DEBUG_PRINT("process", "IO thread exitting");

	return NULL;
}


/**
 * This function runs on it's own thread and
 * waits for processes to exit and then handles
 * the event appropriately.
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
				usleep(500 * 1000L);
				continue;
			}
			fprintf(stderr, "process: wait() returned -1 (errno=%i)\n",
				errno);
			break;
		}

		DEBUG_VPRINT("process", "Process with pid %i exitted", pid);

		pthread_mutex_lock(&process_list_lock);

		LIST_FOREACH_SAFE(struct mb_process*, proc, &process_list, {
			if (proc->pid == pid) {
				DEBUG_VPRINT("process", "Process %i exitted with status %i",
					proc->id, WEXITSTATUS(status));

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

						if (proc->autorestart_delay == 0) {
							/* if the process is set to restart without
							 * delay then restart it now */
							mb_process_autorestart(0, proc);
						} else {
							/* set a timer to restart the process
							 * after a delay */
							struct timespec tv;
							tv.tv_sec = proc->autorestart_delay;
							tv.tv_nsec = 0;
							if (mbt_register(&tv, MB_TIMER_TYPE_AUTORELOAD, -1,
								mb_process_autorestart, proc) == -1) {
								LOG_PRINT(MB_LOGLEVEL_ERROR, "process",
									"Could not register autorestart timer");
							}
						}
						continue;
					}
				}

				/* invoke the process exit callback */
				if (proc->exit_callback != NULL) {
					proc->exit_callback(proc->id,
						WEXITSTATUS(status), proc->exit_callback_data);
				}

				if (proc->flags & MB_PROCESS_WAIT) {
					/* save exit status and wake any threads waiting
					 * on this process */
					DEBUG_VPRINT("process", "Signaling process %i", proc->id);
					proc->exitted = 1;
					proc->exit_status = WEXITSTATUS(status);
					pthread_cond_broadcast(&proc->cond);
				} else {
					DEBUG_VPRINT("process", "Freeing process %i", proc->id);
					/* remove process from list */
					LIST_REMOVE(proc);
					/* cleanup */
					mb_process_free(proc);
				}

				break;
			}
		});

		pthread_mutex_unlock(&process_list_lock);
	}

	io_quit = 1;

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

	if ((proc = mb_process_getbyid(id, 0)) != NULL) {
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
	if ((proc = mb_process_getbyid(id, 0)) == NULL) {
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
 * Set the amount of time, in seconds, to wait for a process
 * to exit after sending SIGTERM before sending SIGKILL.
 */
int
mb_process_setsigkilldelay(int procid, unsigned delay)
{
	struct mb_process * const proc =
		mb_process_getbyid(procid, 0);
	if (proc == NULL) {
		return -ENOENT;
	}
	proc->force_kill_delay = delay;
	return 0;
}


/**
 * mb_process_start() -- Starts and monitors a child process.
 */
int
mb_process_start(const char *binary, const char * const argv[],
	enum mb_process_flags flags, const char *name, mb_process_exit exit_callback,
	void *callback_data)
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
	proc->exit_status = -1;
	proc->exitted = 0;
	proc->stopping = 0;
	proc->flags = flags;
	proc->force_kill_delay = 30;
	proc->autorestart_delay = 5;
	proc->args = mb_process_clone_args(argv);
	proc->name = strdup(name);
	proc->binary = strdup(binary);
	proc->exit_callback = exit_callback;
	proc->exit_callback_data = callback_data;

	/* initialize pthread primitives */
	if (pthread_cond_init(&proc->cond, NULL) != 0) {
		LOG_PRINT(MB_LOGLEVEL_ERROR, "process",
			"Failed to initialize pthread primitives");
		mb_process_free(proc);
	}

	/* check that all memory allocations succeeded */
	if (proc->binary == NULL || proc->args == NULL || proc->name == NULL) {
		LOG_PRINT(MB_LOGLEVEL_ERROR,  "process", "Out of memory");
		mb_process_free(proc);
		return -1;
	}

#ifdef DEBUG
	int l = 0, i;
	char *tmp;
	for (i = 0; proc->args[i] != NULL; i++) {
		l += (strlen(proc->args[i]) * sizeof(char)) + 2;
	}
	if ((tmp = malloc(l)) != NULL) {
		tmp[0] = '\0';
		for (i = 0; proc->args[i] != NULL; i++) {
			strcat(tmp, proc->args[i]);
			strcat(tmp, " ");
		}
		DEBUG_VPRINT("process", "Exec: %s", tmp);
		free(tmp);
	}
#endif

	/* We need to lock the list BEFORE FORKING to ensure that even if
	 * the child dies right away the exit code is caught and processed */
	pthread_mutex_lock(&process_list_lock);
	LIST_ADD(&process_list, proc);
	if ((proc->pid = mb_process_fork(proc)) == -1) {
		LIST_REMOVE(proc);
	}
	ret = proc->id;
	pthread_mutex_unlock(&process_list_lock);

	return ret;
}


/**
 * mb_process_wait() -- Wait for a process to exit.
 *
 * NOTE: This function will fail if MB_PROCESS_WAIT is not set
 * when starting the process!
 */
int
mb_process_wait(int id, int *exit_status)
{
	int ret = -1;
	struct mb_process *proc;

	pthread_mutex_lock(&process_list_lock);

	/* find the process to wait for */
	if ((proc = mb_process_getbyid(id, 1)) == NULL) {
		LOG_VPRINT(MB_LOGLEVEL_ERROR, "process",
			"Cannot wait for process id %i (no such process)", id);
		errno = ENOENT;
		goto end;
	}

	/* if the process doesn't have the wait flag return error */
	if ((proc->flags & MB_PROCESS_WAIT) == 0) {
		errno = EINVAL;
		goto end;
	}

	/* wait for process to exit */
	if (!proc->exitted) {
		DEBUG_VPRINT("process", "Waiting for process %i", proc->id);
		pthread_cond_wait(&proc->cond, &process_list_lock);
	}

	/* at this point the process exitted so the pid must be -1 */
	assert(proc->pid == -1);

	/* return exit status */
	*exit_status = proc->exit_status;

	/* free the process */
	LIST_REMOVE(proc);
	mb_process_free(proc);
	ret = 0;
end:
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

	DEBUG_VPRINT("process", "Stopping process id %i", id);

	if ((proc = mb_process_getbyid(id, 0)) != NULL) {

		DEBUG_VPRINT("process", "Found process %i (pid=%i name='%s')",
			id, proc->pid, proc->name);

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

			/* register a timer to SIGKILL the process if it fails to exit */
			tv.tv_sec = proc->force_kill_delay;
			tv.tv_nsec = 0;
			*id_copy = id;
			if (mbt_register(&tv, MB_TIMER_TYPE_AUTORELOAD, -1, mb_process_force_kill, id_copy) == -1) {
				LOG_PRINT(MB_LOGLEVEL_ERROR, "process",
					"Could not register force stop timer");
				free(id_copy);
				return -1;
			}
		}
		return 0;

	} else {
		LOG_VPRINT_ERROR("Process id %i not found", id);
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

	quit = 0;
	io_quit = 0;

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

	/* set the exit flag and stop all processes */
	quit = 1;
	LIST_FOREACH_SAFE(struct mb_process*, proc, &process_list, {
		if (!proc->stopping || !(proc->flags & MB_PROCESS_WAIT)) {
			mb_process_stop(proc->id);
		}
	});

	/* if any process remains dump them to the log */
	if (LIST_SIZE(&process_list) > 0) {
		DEBUG_VPRINT("process", "Remaining processes: %zd", LIST_SIZE(&process_list));
		LIST_FOREACH(struct mb_process*, proc, &process_list) {
			DEBUG_VPRINT("process", "Process id %i: %s pid=%i waiting=%i stopping=%i",
				proc->id, proc->name, proc->pid,
				(proc->flags & MB_PROCESS_WAIT) ? 1 : 0,
				proc->stopping);
		}

	}

	/* wait for threads */
	DEBUG_PRINT("process", "Waiting for monitor threads");
	pthread_join(monitor_thread, 0);
	pthread_join(io_thread, 0);

	DEBUG_PRINT("process", "Process monitor down");
}
