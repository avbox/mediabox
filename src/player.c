#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "input.h"

#define MPLAYER_BIN "/usr/bin/mplayer"
#define VIDEO_OUTPUT "directfb"

struct mbp
{
	int stdin_fileno;
	const char *media_file;
	pthread_t thread;
};

static void*
run_mplayer_thread(void *arg)
{
	int pid;
	int pipe_stdout[2], pipe_stdin[2];
	struct mbp *inst = (struct mbp*) arg;

	if (pipe(pipe_stdout) == -1 || pipe(pipe_stdin) == -1) {
		fprintf(stderr, "pipe() failed!\n");
		pthread_exit((void*) -1);
	}

	if ((pid = fork()) == -1) {
		fprintf(stderr, "systemui -- fork failed\n");
		pthread_exit((void*)-1);

	} else if (pid == 0) {
		/* replace stdout/stdin */
		if (dup2(pipe_stdout[1], STDOUT_FILENO) == -1) {
			fprintf(stderr, "child -- pipe() failed\n");
			exit(1);
		}
		if (dup2(pipe_stdin[0], STDIN_FILENO) == -1) {
			fprintf(stderr, "child -- dup2() failed (stdin)\n");
			exit(1);
		}

		close(pipe_stdout[0]);
		close(pipe_stdin[1]);

		//mplayer -vo fbdev2 /media/UPnP/MediaTomb/Movies/Avatar\ ECE\ 2009\ 720p.mp4
		execv(MPLAYER_BIN, (char * const[]) { "mplayer",
			"-vo", VIDEO_OUTPUT, strdup(inst->media_file), NULL });
		fprintf(stderr, "systemui -- exec() failed\n");
		exit(1);

	} else { /* parent */
		int ret;
		char buf[1024];

		/* close write end of pipe */
		close(pipe_stdout[1]);
		close(pipe_stdin[0]);

		/* save input file descriptor */
		inst->stdin_fileno = pipe_stdin[1];

		/* read and discard output for now */
		while (1) {
			if ((ret = read(pipe_stdout[0], buf, 1024)) <= 0) {
				if (ret == 0) { /* eof */
					break;
				}
				if (errno == EINTR) {
					continue;
				} else {
					fprintf(stderr, "read() error! errno=%i\n", errno);
				}
			}
		}

		while (waitpid(pid, &ret, 0) == -1) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "waitpid failed! (errno=%i)\n", errno);
		}

		fprintf(stderr, "mplayer exited with status code %i",
			WEXITSTATUS(ret));

		/* close pipe */
		inst->stdin_fileno = -1;
		close(pipe_stdout[0]);
		close(pipe_stdin[1]);

		pthread_exit((void*) &ret);
	}
	return 0;
}

int 
mbp_play(struct mbp *inst, const char * const path)
{
	if (inst == NULL) {
		return -1;
	}

	if (path == NULL) {
		fprintf(stderr, "mbp_play() failed -- NULL path\n");
		return -1;
	}

	inst->media_file = path;

	if (pthread_create(&inst->thread, NULL, run_mplayer_thread, inst) != 0) {
		fprintf(stderr, "pthread_create() failed!\n");
		return -1;
	}
	return 0;
}

int
mbp_stop(struct mbp* inst)
{
	return 0;
}

struct mbp*
mbp_init(struct mbi *input)
{
	struct mbp* inst;

	(void) input;

	inst = malloc(sizeof(struct mbp));
	if (inst == NULL) {
		fprintf(stderr, "mbp_init() failed -- out of memory\n");
		return NULL;
	}

	inst->stdin_fileno = -1;
	inst->media_file = NULL;
	return inst;
}

void
mbp_destroy(struct mbp *inst)
{
	if (inst == NULL) {
		return;
	}
	if (inst->media_file != NULL) {
		free(inst);
	}
}

