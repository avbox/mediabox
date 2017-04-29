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


struct avbox_process;


struct callback_state {
	int result;
	int returned;
	int timer;
	struct avbox_process *process;
};

/**
 * Represents a process managed by avbox.
 */
LISTABLE_STRUCT(avbox_process,
	int id;
	pid_t pid;
	int stdin;
	int stdout;
	int stderr;
	int exit_status;
	int exitted;
	unsigned force_kill_delay;
	unsigned autorestart_delay;
	enum avbox_process_flags flags;
	const char *name;
	const char *binary;
	char * const * args;
	avbox_process_exit exit_callback;
	void *exit_callback_data;
	int stopping;
	struct callback_state *cbstate;
	pthread_cond_t cond;
);


static LIST process_list;
static pthread_mutex_t process_list_lock = PTHREAD_MUTEX_INITIALIZER;
static int nextid = 1;
static pthread_t monitor_thread;
static pthread_t io_thread;
static int quit = 0, io_quit = 0;


/**
 * Checks that only one of the specified flags is set.
 */
static int
avbox_process_checkflagsoneof(enum avbox_process_flags proc_flags, enum avbox_process_flags flags)
{
	int i;
	if (proc_flags & flags) {
		proc_flags &= flags;
		for (i = 0; i < sizeof(enum avbox_process_flags) * CHAR_BIT; i++) {
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
 * Gets a process from the list by it's id.
 */
static struct avbox_process*
avbox_process_getbyid(const int id, const int locked)
{
	struct avbox_process *proc, *ret = NULL;

	if (!locked) {
		pthread_mutex_lock(&process_list_lock);
	}

	LIST_FOREACH(struct avbox_process*, proc, &process_list) {
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
 * Forks and execs a process.
 */
static pid_t
avbox_process_fork(struct avbox_process *proc)
{
	int ret = -1;
	int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 };

	if (pipe(in) == -1 || pipe(out) == -1 || pipe(err) == -1) {
		LOG_VPRINT_ERROR("Could not create pipes: %s", strerror(errno));
		goto end;
	}

	if ((proc->pid = fork()) == -1) {
		LOG_VPRINT_ERROR("Could not fork(): %s", strerror(errno));
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
		LOG_VPRINT_ERROR("Could not create standard file descriptors: %s",
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* close all file descriptors >= 3 */
	closefrom(3);

	/* set the process niceness */
	if (proc->flags & AVBOX_PROCESS_NICE) {
		if (nice(5) == -1) {
			LOG_VPRINT_ERROR("Could not set process niceness: %s",
				strerror(errno));
		}
	}

#ifdef ENABLE_IONICE
	/* set the process IO priority */
	if (proc->flags & AVBOX_PROCESS_IONICE_IDLE) {
		if (avbox_gainroot() == 0) {
			if (ioprio_set(IOPRIO_WHO_PROCESS, getpid(), IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0)) == -1) {
				LOG_VPRINT_ERROR("Could not set process IO priority to IDLE: %s",
					strerror(errno));
			}
			avbox_droproot();
		}
	} else if (proc->flags & AVBOX_PROCESS_IONICE_BE) {
		if (avbox_gainroot() == 0) {
			if (ioprio_set(IOPRIO_WHO_PROCESS, getpid(), IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 0)) == -1) {
				LOG_VPRINT_ERROR("Could not set process IO priority to BE: %s",
					strerror(errno));
			}
			avbox_droproot();
		}
	}
#endif

	/* if the process requires root then elevate privilege */
	if (proc->flags & AVBOX_PROCESS_SUPERUSER) {
		if (avbox_gainroot() == -1) {
			LOG_VPRINT_ERROR("Could not gain root for process '%s' (id=%i)",
				proc->name, proc->id);
		}
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
 * Clones a list of arguments.
 */
static char * const*
avbox_process_clone_args(const char * const argv[])
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
 * Free's a cloned list of arguments.
 */
static void
avbox_process_free_args(char * const argv[]) {
	int i = 0;
	while (argv[i] != NULL) {
		free(argv[i++]);
	}
	free((void*) argv);
}


/**
 * Free a process structure.
 */
static void
avbox_process_free(struct avbox_process *proc)
{
	assert(proc != NULL);

	if (proc->name != NULL) {
		free((void*) proc->name);
	}
	if (proc->binary != NULL) {
		free((void*) proc->binary);
	}
	if (proc->args != NULL) {
		avbox_process_free_args(proc->args);
	}
	if (proc->cbstate != NULL) {
		free(proc->cbstate);
	}
	free(proc);
}


/**
 * Gets the next process id.
 */
static inline int
avbox_process_get_next_id(void)
{
	return nextid++;
}


/**
 * This is the handler a timer that is set when
 * avbox_process_stop() is called and the process does not have the
 * AVBOX_PROCESS_SIGKILL flag set. It fires 5 seconds after stop() returns
 * and will SIGKILL the process (in case SIGTERM didn't work).
 */
static enum avbox_timer_result
avbox_process_force_kill(int id, void *data)
{
	int proc_id = *((int*) data);
	struct avbox_process *proc;
	enum avbox_timer_result ret = AVBOX_TIMER_CALLBACK_RESULT_STOP;

	DEBUG_VPRINT("process", "Force kill callback for process %id",
		proc_id);

	pthread_mutex_lock(&process_list_lock);

	LIST_FOREACH(struct avbox_process*, proc, &process_list) {
		if (proc->id == proc_id) {
			DEBUG_VPRINT("process", "Force killing process %i (pid=%i)",
				proc_id, proc->pid);
			if (kill(proc->pid, SIGKILL) == -1) {
				LOG_PRINT_ERROR("kill() regurned -1");
			}
			ret = AVBOX_TIMER_CALLBACK_RESULT_CONTINUE;
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
static enum avbox_timer_result
avbox_process_autorestart(int id, void *data)
{
	struct avbox_process * const proc = (struct avbox_process * const) data;

	DEBUG_VPRINT("process", "Restarting process %s (id=%i pid=%i)",
		proc->name, proc->id, proc->pid);
	if ((proc->pid = avbox_process_fork(proc)) == -1) {
		LOG_VPRINT_ERROR("Could not restart process '%s' (id=%i)",
			proc->name, proc->id);
	} else {
		DEBUG_VPRINT("process", "Process %s restarted. New pid=%i",
			proc->name, proc->pid);
	}
	return AVBOX_TIMER_CALLBACK_RESULT_STOP;
}


/**
 * Runs on it's own thread and handles standard IO
 * to/from processes.
 */
static void *
avbox_process_io_thread(void *arg)
{
	fd_set fds;
	int fd_max, res;
	char buf[1024];
	struct avbox_process *proc;
	struct timeval tv;

	MB_DEBUG_SET_THREAD_NAME("proc-io");
	DEBUG_PRINT("process", "Starting IO thread");

	while (!io_quit) {

		fd_max = 0;
		FD_ZERO(&fds);

		/* build a file descriptor set to select() */
		pthread_mutex_lock(&process_list_lock);
		LIST_FOREACH(struct avbox_process*, proc, &process_list) {
			if (proc->flags & AVBOX_PROCESS_STDOUT_LOG) {
				if (proc->stdout != -1) {
					FD_SET(proc->stdout, &fds);
					fd_max = MAX(fd_max, proc->stdout);
				}
			}
			if (proc->flags & AVBOX_PROCESS_STDERR_LOG) {
				if (proc->stderr != -1) {
					FD_SET(proc->stderr, &fds);
					fd_max = MAX(fd_max, proc->stderr);
				}
			}
		}
		pthread_mutex_unlock(&process_list_lock);

		/* if no file descriptors to monitor sleep
		 * for 1/2 second */
		if (fd_max == 0) {
			usleep(500L * 1000L);
			continue;
		}

		/* select the output file descriptors of all processes */
		tv.tv_sec = 0;
		tv.tv_usec = 500 * 1000L;
		if ((res = select(fd_max + 1, &fds, NULL, NULL, &tv)) == 0) {
			continue;
		} else if (res < 0) {
			if (errno == EINTR) {
				continue;
			}
			LOG_VPRINT_ERROR("select() failed: %s (errno=%i)",
				strerror(errno), errno);
			usleep(500 * 1000L);
			continue;
		}

		/* process all pending output */
		pthread_mutex_lock(&process_list_lock);
		LIST_FOREACH(struct avbox_process*, proc, &process_list) {
			if (proc->flags & AVBOX_PROCESS_STDOUT_LOG) {
				if (proc->stdout != -1 && FD_ISSET(proc->stdout, &fds)) {
					if ((res = read(proc->stdout, buf, sizeof(buf))) == -1) {
						LOG_VPRINT_ERROR("Could not read process STDOUT: %s",
							strerror(errno));
						continue;
					}
					if (proc->flags & AVBOX_PROCESS_STDOUT_LOG) {
						/* TODO: We need to break the output in lines */
						LOG_VPRINT_ERROR("%s: %s", proc->name, buf);
					}
				}
			}
			if (proc->flags & AVBOX_PROCESS_STDERR_LOG) {
				if (proc->stderr != -1 && FD_ISSET(proc->stderr, &fds)) {
					if ((res = read(proc->stderr, buf, sizeof(buf))) == -1) {
						LOG_VPRINT_ERROR("Could not read process STDERR: %s",
							strerror(errno));
						continue;
					}
					if (proc->flags & AVBOX_PROCESS_STDERR_LOG) {
						/* TODO: We need to break the output in lines */
						LOG_VPRINT_ERROR("%s: %s", proc->name, buf);
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
 * Invokes the callback function from another thread.
 */
static enum avbox_timer_result
avbox_process_callback_helper(int id, void *data)
{
	struct avbox_process * const proc =
		(struct avbox_process*) data;
	proc->cbstate->result = proc->exit_callback(proc->id,
		proc->exit_status, proc->exit_callback_data);
	proc->cbstate->returned = 1;
	return AVBOX_TIMER_CALLBACK_RESULT_STOP;
}


/**
 * This function runs on it's own thread and
 * waits for processes to exit and then handles
 * the event appropriately.
 */
static void *
avbox_process_monitor_thread(void *arg)
{
	pid_t pid;
	int status, found = 0, cbresult;
	struct avbox_process *proc;

	MB_DEBUG_SET_THREAD_NAME("proc-mon");
	DEBUG_PRINT("process", "Starting process monitor thread");

	while (!quit || LIST_SIZE(&process_list) > 0) {
		if ((pid = waitpid(-1, &status, WNOHANG)) <= 0) {
			if (errno == EINTR) {
				usleep(500 * 1000L);
			} else if (errno == ECHILD) {
				usleep(500 * 1000L);
			} else {
				LOG_VPRINT_ERROR("Could not wait() for process: %s",
					strerror(errno));
				break;
			}

			/* since processes may have pid set to -1
			 * we need to change to something else to avoid
			 * a false match bellow */
			pid = -2;
		}


		pthread_mutex_lock(&process_list_lock);

		LIST_FOREACH_SAFE(struct avbox_process*, proc, &process_list, {
			if (proc->pid == pid || proc->cbstate != NULL) {
				if (proc->pid == pid) {

					assert(proc->cbstate == NULL);

					/* close file descriptors */
					if (proc->stdin != -1) close(proc->stdin);
					if (proc->stdout != -1) close(proc->stdout);
					if (proc->stderr != -1) close(proc->stderr);

					/* clear file descriptors and PID */
					proc->pid = -1;
					proc->stdin = -1;
					proc->stdout = -1;
					proc->stderr = -1;

					/* save exit status */
					proc->exit_status = WEXITSTATUS(status);

					/* if the process terminated abnormally then log
					 * an error message */
					if (proc->exit_status) {
						LOG_VPRINT_WARN("Process '%s' exitted with status %i (id=%i,pid=%i)",
							proc->name, proc->exit_status, proc->id, pid);
					} else {
						DEBUG_VPRINT("process", "Process '%s' exitted with status %i (id=%i,pid=%i)",
							proc->name, proc->exit_status, proc->id, pid);
					}

					/* if we have a callback function invoke it from another
					 * thread and continue */
					if (proc->exit_callback != NULL) {
						if ((proc->cbstate = malloc(sizeof(struct callback_state))) == NULL) {
							LOG_PRINT_ERROR("Could not allocate callback state. Aborting");
							abort();
						}
						proc->cbstate->result = 0;
						proc->cbstate->returned = 0;

						/* for now we use a 0 interval oneshot timer to invoke the
						 * callback from another thread */
						struct timespec tv;
						tv.tv_sec = 0;
						tv.tv_nsec = 0;
						if ((proc->cbstate->timer = avbox_timer_register(&tv,
							AVBOX_TIMER_TYPE_ONESHOT, -1, avbox_process_callback_helper, proc)) == -1) {
							LOG_PRINT_ERROR("Could not fire callback!");
							abort();
						}
						continue;
					} else {
						cbresult = 0;
					}
				} else if (proc->cbstate != NULL) {
					if (!proc->cbstate->returned) {
						continue;
					}

					/* at this point we're back from the callback
					 * so we can continue */
					cbresult = proc->cbstate->result;
					free(proc->cbstate);
					proc->cbstate = NULL;
				}

				/* if the process terminated abormally and the AUTORESTART flag is
				 * set then restart the process */
				if (cbresult == 0 && ((proc->flags & AVBOX_PROCESS_AUTORESTART_ALWAYS) != 0 ||
					(proc->exit_status != 0 && (proc->flags & AVBOX_PROCESS_AUTORESTART) != 0))) {
					if (!proc->stopping) {
						LOG_VPRINT_INFO("Auto restarting process '%s' (id=%i,pid=%i)",
							proc->name, proc->id, pid);

						if (proc->autorestart_delay == 0) {
							/* if the process is set to restart without
							 * delay then restart it now */
							avbox_process_autorestart(0, proc);
						} else {
							/* set a timer to restart the process
							 * after a delay */
							struct timespec tv;
							tv.tv_sec = proc->autorestart_delay;
							tv.tv_nsec = 0;
							if (avbox_timer_register(&tv, AVBOX_TIMER_TYPE_AUTORELOAD, -1,
								avbox_process_autorestart, proc) == -1) {
								LOG_PRINT_ERROR("Could not register autorestart timer");
							}
						}
						continue;
					}
				}

				if (proc->flags & AVBOX_PROCESS_WAIT) {
					/* save exit status and wake any threads waiting
					 * on this process */
					proc->exitted = 1;
					pthread_cond_broadcast(&proc->cond);
				} else {
					DEBUG_VPRINT("process", "Freeing process %i", proc->id);
					/* remove process from list */
					LIST_REMOVE(proc);
					/* cleanup */
					avbox_process_free(proc);
				}

				found = 1;
				continue;
			}
		});

		pthread_mutex_unlock(&process_list_lock);

		/* if the process was not found log an error */
		if (!found) {
			LOG_VPRINT_ERROR("Unmanaged process with pid %i exitted", pid);
		}
	}

	io_quit = 1;

	return NULL;
}


/**
 * Gets the pid of a process.
 *
 * NOTE: The PID cannot be used to identify a process using the process
 * API since a process may crash or get restarted and get a new PID.
 */
pid_t
avbox_process_getpid(int id)
{
	struct avbox_process *proc;

	if ((proc = avbox_process_getbyid(id, 0)) != NULL) {
		return proc->pid;
	}
	return -1;
}


/**
 * Opens one of the standard file descriptors for
 * the process.
 *
 * NOTE: After opening a file descriptor with this function the process
 * manager stops managing the file descriptor so you must call close()
 * on it when you're done using it.
 */
int
avbox_process_openfd(int id, int std_fileno)
{
	int result = -1;
	struct avbox_process *proc;

	assert(std_fileno == STDIN_FILENO ||
		std_fileno == STDOUT_FILENO ||
		std_fileno == STDERR_FILENO);

	/* get the process object */
	if ((proc = avbox_process_getbyid(id, 0)) == NULL) {
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
avbox_process_setsigkilldelay(int procid, unsigned delay)
{
	struct avbox_process * const proc =
		avbox_process_getbyid(procid, 0);
	if (proc == NULL) {
		return -ENOENT;
	}
	proc->force_kill_delay = delay;
	return 0;
}


/**
 * Starts and monitors a child process.
 */
int
avbox_process_start(const char *binary, const char * const argv[],
	enum avbox_process_flags flags, const char *name, avbox_process_exit exit_callback,
	void *callback_data)
{
	int ret = -1;
	struct avbox_process *proc;

	assert(binary != NULL);
	assert(argv != NULL);
	assert(name != NULL);

	/* check for conflicting IO priority flags */
	if (avbox_process_checkflagsoneof(flags, AVBOX_PROCESS_IONICE) == -1) {
		LOG_PRINT_ERROR("Multiple IO priorities set!");
		return -1;
	}

	/* check for conflicting STDOUT flags */
	if (avbox_process_checkflagsoneof(flags, AVBOX_PROCESS_STDOUT) == -1) {
		LOG_PRINT_ERROR("Multiple STDOUT flags set!");
		return -1;
	}

	/* check for conflicting STDERR flags */
	if (avbox_process_checkflagsoneof(flags, AVBOX_PROCESS_STDERR) == -1) {
		LOG_PRINT_ERROR("Multiple STDERR flags set!");
		return -1;
	}

	/* allocate memory for process structure */
	if ((proc = malloc(sizeof(struct avbox_process))) == NULL) {
		LOG_PRINT_ERROR("Could not create process: Out of memory");
		return -1;
	}

	/* initialize process structure and add it to list */
	proc->id = avbox_process_get_next_id();
	proc->stdin = -1;
	proc->stdout = -1;
	proc->stderr = -1;
	proc->exit_status = -1;
	proc->exitted = 0;
	proc->stopping = 0;
	proc->flags = flags;
	proc->force_kill_delay = 30;
	proc->autorestart_delay = 5;
	proc->args = avbox_process_clone_args(argv);
	proc->name = strdup(name);
	proc->binary = strdup(binary);
	proc->exit_callback = exit_callback;
	proc->exit_callback_data = callback_data;
	proc->cbstate = NULL;

	/* initialize pthread primitives */
	if (pthread_cond_init(&proc->cond, NULL) != 0) {
		LOG_PRINT_ERROR("Failed to initialize pthread primitives");
		avbox_process_free(proc);
	}

	/* check that all memory allocations succeeded */
	if (proc->binary == NULL || proc->args == NULL || proc->name == NULL) {
		LOG_PRINT_ERROR("Could not create process: Out of memory");
		avbox_process_free(proc);
		return -1;
	}

#ifndef NDEBUG
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
		DEBUG_VPRINT("process", "Starting process: '%s' (id=%i)",
			tmp, proc->id);
		free(tmp);
	}
#endif

	/* We need to lock the list BEFORE FORKING to ensure that even if
	 * the child dies right away the exit code is caught and processed */
	pthread_mutex_lock(&process_list_lock);
	LIST_ADD(&process_list, proc);
	if ((proc->pid = avbox_process_fork(proc)) == -1) {
		LIST_REMOVE(proc);
	}
	ret = proc->id;
	pthread_mutex_unlock(&process_list_lock);

	return ret;
}


/**
 * Wait for a process to exit.
 *
 * NOTE: This function will fail if AVBOX_PROCESS_WAIT is not set
 * when starting the process!
 */
int
avbox_process_wait(int id, int *exit_status)
{
	int ret = -1;
	struct avbox_process *proc;

	pthread_mutex_lock(&process_list_lock);

	/* find the process to wait for */
	if ((proc = avbox_process_getbyid(id, 1)) == NULL) {
		LOG_VPRINT_ERROR("Cannot wait for process id %i (no such process)", id);
		errno = ENOENT;
		goto end;
	}

	/* if the process doesn't have the wait flag return error */
	if ((proc->flags & AVBOX_PROCESS_WAIT) == 0) {
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
	DEBUG_VPRINT("process", "Freeing process %i",
		proc->id);
	LIST_REMOVE(proc);
	avbox_process_free(proc);
	ret = 0;
end:
	pthread_mutex_unlock(&process_list_lock);
	return ret;
}


/**
 * Stops a running child process.
 */
int
avbox_process_stop(int id)
{
	struct avbox_process *proc;

	DEBUG_VPRINT("process", "Stopping process id %i", id);

	if ((proc = avbox_process_getbyid(id, 0)) != NULL) {

		DEBUG_VPRINT("process", "Found process %i (pid=%i name='%s')",
			id, proc->pid, proc->name);

		proc->stopping = 1;

		if (proc->flags & AVBOX_PROCESS_SIGKILL) {
			/* send SIGKILL to the process */
			if (kill(proc->pid, SIGKILL) == -1) {
				/* TODO: Is this guaranteed to succeed?
				 * Should we abort() here? */
				LOG_VPRINT_ERROR("kill(pid, SIGKILL) returned -1 (errno=%i)", errno);
				return -1;
			}
		} else {
			struct timespec tv;
			int *id_copy;

			/* allocate heap memory for a copy of the id */
			if ((id_copy = malloc(sizeof(int))) == NULL) {
				LOG_PRINT_ERROR("Cannot kill service: Out of memory");
				return -1;
			}

			/* send SIGTERM to the process */
			if (kill(proc->pid, SIGTERM) == -1) {
				LOG_VPRINT_ERROR("Could not send SIGTERM: %s", strerror(errno));
				return -1;
			}

			/* register a timer to SIGKILL the process if it fails to exit */
			tv.tv_sec = proc->force_kill_delay;
			tv.tv_nsec = 0;
			*id_copy = id;
			if (avbox_timer_register(&tv, AVBOX_TIMER_TYPE_AUTORELOAD, -1, avbox_process_force_kill, id_copy) == -1) {
				LOG_PRINT_ERROR("Could not register force stop timer");
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
 * Initialize the process manager.
 */
int
avbox_process_init(void)
{
	DEBUG_PRINT("process", "Initializing process monitor");

	LIST_INIT(&process_list);

	quit = 0;
	io_quit = 0;

	if (pthread_create(&monitor_thread, NULL, avbox_process_monitor_thread, NULL) != 0) {
		LOG_PRINT_ERROR("Could not create monitor thread!");
		return -1;
	}

	if (pthread_create(&io_thread, NULL, avbox_process_io_thread, NULL) != 0) {
		LOG_PRINT_ERROR("Could not create IO thread!");
		quit = 1;
		pthread_join(io_thread, 0);
		return -1;
	}

	return 0;
}


/**
 * Shutdown the process manager.
 */
void
avbox_process_shutdown(void)
{
	struct avbox_process *proc;

	DEBUG_PRINT("process", "Shutting down process monitor");

	/* set the exit flag and stop all processes */
	quit = 1;
	LIST_FOREACH_SAFE(struct avbox_process*, proc, &process_list, {
		if (!proc->stopping || !(proc->flags & AVBOX_PROCESS_WAIT)) {
			avbox_process_stop(proc->id);
		}
	});

	/* if any process remains dump them to the log */
	if (LIST_SIZE(&process_list) > 0) {
		DEBUG_VPRINT("process", "Remaining processes: %zd", LIST_SIZE(&process_list));
		LIST_FOREACH(struct avbox_process*, proc, &process_list) {
			DEBUG_VPRINT("process", "Process id %i: %s pid=%i waiting=%i stopping=%i",
				proc->id, proc->name, proc->pid,
				(proc->flags & AVBOX_PROCESS_WAIT) ? 1 : 0,
				proc->stopping);
		}

	}

	/* wait for threads */
	DEBUG_PRINT("process", "Waiting for monitor threads");
	pthread_join(monitor_thread, 0);
	pthread_join(io_thread, 0);

	DEBUG_PRINT("process", "Process monitor down");
}
