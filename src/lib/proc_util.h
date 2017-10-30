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
