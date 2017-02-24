#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "debug.h"


/**
 * Gets the path of the current process' executable image.
 */
ssize_t
mb_getexepath(char *buf, size_t bufsize)
{
	ssize_t ret;
	char *exe_symlink;
	const char *exe_symlink_pattern = "/proc/%i/exe";
	const int pid_length = (int) (log10(LONG_MAX) + 1);
	const int exe_path_length = strlen(exe_symlink_pattern) + pid_length - 1;

	exe_symlink = malloc(sizeof(char) * (exe_path_length + 1));
	if (exe_symlink == NULL) {
		errno = ENOMEM;
		return -1;
	}

	snprintf(exe_symlink, exe_path_length + 1, exe_symlink_pattern, getpid());

	DEBUG_VPRINT("proc_util", "Reading symlink: %s", exe_symlink);

	ret = readlink(exe_symlink, buf, bufsize);
	free(exe_symlink);
	return ret;
}
