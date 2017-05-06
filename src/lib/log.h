/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
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


/**
 * Initialize logging system for early logging
 */
void
log_init();

#endif
