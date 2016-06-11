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

#define DELUGE_BIN "/usr/bin/deluge-console"
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
		if ((fdr = open(src, O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)) != -1) {
			if ((fdw = open(dst, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)) != -1) {
				if (sendfile(fdw, fdr, 0, st.st_size) != -1) {
					ret = 0;
				} else {
					fprintf(stderr, "download-manager: sendfile() failed. errno=%i\n", errno);
				}
				close(fdw);
			} else {
				fprintf(stderr, "download-manager: Could not open %s\n", dst);
			}
			close(fdr);
		} else {
			fprintf(stderr, "download-manager: Could not open %s (errno=%i)\n", src, errno);
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
			if (nice(5) == -1) {
				fprintf(stderr, "downloads: I'm trying to be nice but I can't. (errno=%i)\n",
					errno);
			}

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
mb_downloadmanager_addurl(char *url)
{
	pid_t pid;

	if ((pid = fork()) == -1) {
		fprintf(stderr, "downloads-backend: fork() failed\n");
		return -1;

	} else if (pid == 0) { /* child */
		execv(DELUGE_BIN, (char * const[]) {
			strdup("deluge-console"),
			strdup("connect"),
			strdup("127.0.0.1"),
			strdup("mediabox"),
			strdup("mediabox;"),
			strdup("add"),
			strdup(url),
			NULL });
		exit(EXIT_FAILURE);

	} else { /* parent */
		int ret;
		while (waitpid(pid, &ret, 0) == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				fprintf(stderr, "downloads-backend: waitpid() failed\n");
				break;
			}
		}
		return ret;
	}
}


int
mb_downloadmanager_init(void)
{
	umask(000);
	mkdir("/var/lib/mediabox", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	mkdir("/var/lib/mediabox/deluge", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	mkdir("/var/lib/mediabox/deluge/plugins", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

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
