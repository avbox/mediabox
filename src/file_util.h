/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __FILE_UTIL_H__
#define __FILE_UTIL_H__


/**
 * Close all file descriptors >= fd_max.
 */
int
closefrom(int fd_max);


/**
 * Copies a file
 */
int
cp(const char *src, const char *dst);


/**
 * Create a directory recursively.
 */
int
mkdir_p(const char * const path, mode_t mode);


/**
 * Get the data directory. If we're running from
 * the source directory use res/ as the data directory.
 * Otherwise use the configured DATADIR.
 */
char *
mb_getdatadir(char *buf, size_t bufsize);


/**
 * Copies a file from ifilename to ofilename replacing
 * all occurrences of match with replace.
 */
int
frep(const char * const ifilename,
	const char * ofilename,
	const char * const match[],
	const char * const replace[]);


#endif
