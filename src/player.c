#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "input.h"
#include "player.h"

#define MPLAYER_BIN "/usr/bin/mplayer"
#define VIDEO_OUTPUT "directfb:noinput:dfbopts=force-windowed"


struct mbp
{
	int stdin_fileno;
	const char *media_file;
	enum mb_player_status status;
	pthread_t thread;
};


static void
mb_player_process_status(struct mbp *inst, const char *status)
{
	if (!memcmp("A:", status, 2 * sizeof(char))) {
		inst->status = MB_PLAYER_STATUS_PLAYING;
	} else if (!memcmp("  =====  PAUSE  =====", status, 21)) {
		fprintf(stderr, "mb_player: Paused\n");
		inst->status = MB_PLAYER_STATUS_PAUSED;
	} else {
		fprintf(stderr, "mb_player: Processing status: '%s'\n",
			status);
	}
}


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
		/* get root acccess */
		if (seteuid(0) == -1 || setegid(0) == -1) {
			fprintf(stderr, "mb_player: Could not get root access\n");
		}

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
			"-vo", VIDEO_OUTPUT,
			strdup(inst->media_file), NULL });
		fprintf(stderr, "systemui -- exec() failed\n");
		exit(1);

	} else { /* parent */
		int ret, bufindex = 0;
		char buf[1024];
		char linebuf[255];

		/* close write end of pipe */
		close(pipe_stdout[1]);
		close(pipe_stdin[0]);

		/* save input file descriptor */
		inst->status = MB_PLAYER_STATUS_PLAYING;
		inst->stdin_fileno = pipe_stdin[1];

		/* read and discard output for now */
		while (1) {
			ssize_t bytes_read;
			if ((bytes_read = read(pipe_stdout[0], buf, 1024)) <= 0) {
				if (ret == 0) { /* eof */
					break;
				}
				if (errno == EINTR) {
					continue;
				} else {
					fprintf(stderr, "read() error! errno=%i\n", errno);
				}
			} else {
				int i;
				for (i = 0; i < bytes_read; i++) {
					if (buf[i] == '\n') {
						continue;
					} else if (buf[i] == '\r') {
						if (bufindex < 255) {
							linebuf[bufindex++] = '\0';
						} else {
							linebuf[254] = '\0';
						}
						mb_player_process_status(inst, linebuf);
						bufindex = 0;
					} else {
						if (bufindex < 255) {
							linebuf[bufindex++] = buf[i];
						}
					}
				}
			}
		}

		/* at this point the player should've exited */
		while (waitpid(pid, &ret, 0) == -1) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "waitpid failed! (errno=%i)\n", errno);
		}

		fprintf(stderr, "mplayer exited with status code %i",
			WEXITSTATUS(ret));

		inst->stdin_fileno = -1;
		inst->status = MB_PLAYER_STATUS_READY;

		/* close pipes */
		close(pipe_stdout[0]);
		close(pipe_stdin[1]);

		pthread_exit((void*) &ret);
	}
	return 0;
}


enum mb_player_status
mb_player_getstatus(struct mbp *inst)
{
	return inst->status;
}


/**
 * mb_player_update() -- Redraw the media player window
 */
void
mb_player_update(struct mbp *inst)
{
	assert(inst != NULL);

	/* this is a hack for now to get the media player window to update */
	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		mbp_play(inst, NULL);
		while (inst->status == MB_PLAYER_STATUS_PAUSED);
		mbp_pause(inst);
	}
}


int 
mbp_play(struct mbp *inst, const char * const path)
{
	if (inst == NULL) {
		return -1;
	}

	if (path == NULL) {
		if (inst->status == MB_PLAYER_STATUS_PAUSED) {
			char comm = 'p';
			return (write_or_epipe(inst->stdin_fileno, &comm, 1) != 1);
		}
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
mbp_pause(struct mbp* inst)
{
	char comm = 'p';

	assert(inst != NULL);

	switch (inst->status) {
	case MB_PLAYER_STATUS_PAUSED: return 0;
	case MB_PLAYER_STATUS_PLAYING:
		assert(inst->stdin_fileno != -1);
		if (write_or_epipe(inst->stdin_fileno, &comm, 1) == 1) {
			while (inst->status != MB_PLAYER_STATUS_PAUSED);
			return 0;
		}
	default:
		return -1;
	}
}


int
mbp_stop(struct mbp* inst)
{
	return 0;
}

struct mbp*
mbp_init(void)
{
	struct mbp* inst;
	inst = malloc(sizeof(struct mbp));
	if (inst == NULL) {
		fprintf(stderr, "mbp_init() failed -- out of memory\n");
		return NULL;
	}

	inst->stdin_fileno = -1;
	inst->media_file = NULL;
	inst->status = MB_PLAYER_STATUS_READY;
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

