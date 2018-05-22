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


#ifndef __MB_LOG_H__
#define __MB_LOG_H__

#include <stdio.h>


enum mb_loglevel
{
	MB_LOGLEVEL_INFO,
	MB_LOGLEVEL_WARN,
	MB_LOGLEVEL_ERROR,
	MB_LOGLEVEL_FATAL
};


/**
 * LOG_VPRINT() - Variadic log print macro.
 */
#define LOG_VPRINT(loglevel, module, fmt, ...) log_printf(module ": " fmt "\n", __VA_ARGS__)


/**
 * LOG_PRINT() -- Log print macro.
 */
#define LOG_PRINT(loglevel, module, str) log_printf(module ": " str "\n");


/* Some convenience macros */
#define LOG_PRINT_ERROR(str) LOG_PRINT(MB_LOGLEVEL_ERROR, LOG_MODULE, str)
#define LOG_PRINT_WARN(str) LOG_PRINT(MB_LOGLEVEL_WARN, LOG_MODULE, str)
#define LOG_PRINT_INFO(str) LOG_PRINT(MB_LOGLEVEL_INFO, LOG_MODULE, str)
#define LOG_VPRINT_ERROR(fmt, ...) LOG_VPRINT(MB_LOGLEVEL_ERROR, LOG_MODULE, fmt, __VA_ARGS__)
#define LOG_VPRINT_WARN(fmt, ...) LOG_VPRINT(MB_LOGLEVEL_WARN, LOG_MODULE, fmt, __VA_ARGS__)
#define LOG_VPRINT_INFO(fmt, ...) LOG_VPRINT(MB_LOGLEVEL_INFO, LOG_MODULE, fmt, __VA_ARGS__)


/**
 * Sets the log file.
 */
void
log_setfile(FILE * const f);


/**
 * This function works just like printf() but it writes to the log
 * file instead of stdout.
 */
size_t
log_printf(const char * fmt, ...);


void
log_backtrace(void);


/**
 * Initialize logging system for early logging
 */
void
log_init();

#endif
