/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_PROC_UTIL_H__
#define __MB_PROC_UTIL_H__

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
avbox_execargs(const char * const filepath, ...);


/**
 * Gets the path of the current process' executable image.
 */
ssize_t
mb_getexepath(char *buf, size_t bufsize);


#endif
