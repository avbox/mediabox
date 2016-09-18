#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>


#include "string_util.h"


/**
 * closefrom() -- Close all file descriptors >= fd_max.
 */
int
closefrom(int fd_max)
{
	DIR *dir;
	struct dirent *ent;
	int fd;

	/* Should work on Linux, Solaris, AIX, Cygwin, NetBSD */

	if ((dir = opendir("/proc/self/fd")) == NULL) {
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		if (strisdigit(ent->d_name)) {
			fd = atoi(ent->d_name);
			if (fd >= fd_max) {
				close(fd);
			}
		}
	}

	closedir(dir);

	return 0;
}
