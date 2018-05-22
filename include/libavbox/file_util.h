/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
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
char *
avbox_getdatadir(char *buf, size_t bufsize);


/**
 * Gets the state directory (usually /var/lib/mediabox)
 */
char *
getstatedir();


/**
 * Copies a file from ifilename to ofilename replacing
 * all occurrences of match with replace.
 */
int
frep(const char * const ifilename,
	const char * ofilename,
	const char * const match[],
	const char * const replace[]);


char*
avbox_get_resource(const char*const res, int *sz);


#endif
