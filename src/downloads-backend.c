#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#define DELUGED_BIN "/usr/bin/deluged"
#define PREFIX "/usr/local"

static int deluge_quit = 0;
static pthread_t deluge_monitor;
static pid_t deluge_pid;


static int
cp(const char *src, const char *dst)
{
	int fdr, fdw, ret = -1;
	struct stat st;

	if (stat(src, &st) == 0) {
		if ((fdr = open(src, O_RDONLY)) != -1) {
			if ((fdw = open(dst, O_WRONLY)) != -1) {
				if (sendfile(fdw, fdr, 0, st.st_size) != -1) {
					ret = 0;
				} else {
					fprintf(stderr, "download-manager: Could not save core config\n");
				}
				close(fdw);
			}
			close(fdr);
		}
	}
	return ret;
}


static void *
mb_downloadmanager_deluged(void *data)
{
	pid_t pid;

	while (!deluge_quit) {

		if ((pid = fork()) == -1) {
			fprintf(stderr, "download-backend: fork() failed\n");
			return NULL;
		} else if (pid == 0) { /* child */
			execv(DELUGED_BIN, (char * const[]) {
				strdup("deluged"),
				strdup("-d"),
				strdup("-p"),
				strdup("58846"),
				strdup("-c"),
				strdup("/var/lib/mediabox/deluge/"),
				NULL });
			exit(EXIT_FAILURE);
		} else {
			int ret;
			deluge_pid = pid;
			while (waitpid(deluge_pid, &ret, 0) == -1) {
				if (errno == EINTR) {
					continue;
				} else {
					fprintf(stderr, "download-backend: waitpid() returned -1 errno=%i\n", errno);
				}
			}
			fprintf(stderr, "download-backend: deluged exited with %i\n", ret);
			deluge_pid = -1;
		}
	}
	return NULL;
}


int
mb_downloadmanager_init(void)
{
	mkdir("/var/lib/mediabox", 777);
	mkdir("/var/lib/mediabox/deluge", 777);
	cp("/usr/local/share/mediabox/deluge-core.conf", "/var/lib/mediabox/deluge/core.conf");
	cp("/usr/local/share/mediabox/deluge-auth", "/var/lib/mediabox/deluge/auth");
	unlink("/var/lib/mediabox/deluge/deluged.pid");


	deluge_quit = 0;
	if (pthread_create(&deluge_monitor, NULL, mb_downloadmanager_deluged, NULL) != 0) {
		fprintf(stderr, "download-backend: pthread_create() failed\n");
		return -1;
	}
	return 0;
}


void
mb_downloadmanager_destroy(void)
{
	deluge_quit = 1;
	if (kill(deluge_pid, SIGKILL) == -1) {
		fprintf(stderr, "download-backend: kill() failed. errno=%i\n", errno);
	}
	pthread_join(deluge_monitor, NULL);
}
