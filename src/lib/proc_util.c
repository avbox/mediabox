/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <assert.h>

#define LOG_MODULE "proc_util"

#include "log.h"
#include "debug.h"
#include "process.h"


/**
 * Takes in an executable name and a variable list
 * of of char * arguments terminated by a NULL string
 * pointer and executes and waits for the command to
 * exit and returns the process' exit code.
 *
 * NOTE: If no arguments are needed you must still
 * pass the NULL terminator argument!
 */
int
avbox_execargs(const char * const filepath, ...)
{
	int i, proc_tmp, ret_tmp = -1;
	const char *args[8] = { filepath, NULL };
	va_list va;

	va_start(va, filepath);

	for (i = 1; i < 8; i++) {
		args[i] = va_arg(va, const char*);
		if (args[i] == NULL) {
			break;
		}
	}

	va_end(va);

	assert(i < 8);
	assert(args[i] == NULL);

	if ((proc_tmp = avbox_process_start(filepath, args,
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT,
		filepath, NULL, NULL)) > 0) {
		avbox_process_wait(proc_tmp, &ret_tmp);
	}

	return ret_tmp;
}



/**
 * Gets the path of the current process' executable image.
 * If the path does not fit in the buffer it will be truncated
 * but still null-terminated.
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
		LOG_PRINT_ERROR("Could not get executable path: Out of memory");
		/* errno = ENOMEM; */
		return -1;
	}

	snprintf(exe_symlink, exe_path_length + 1, exe_symlink_pattern, getpid());

	DEBUG_VPRINT("proc_util", "Reading symlink: %s", exe_symlink);

	if ((ret = readlink(exe_symlink, buf, bufsize)) > 0) {
		if (ret == bufsize) {
			buf[--ret] = '\0';
		} else {
			buf[ret] = '\0';
		}
	}
	free(exe_symlink);
	return ret;
}
