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
#define LOG_VPRINT(loglevel, module, fmt, ...) fprintf(stderr, module ": " fmt "\n", __VA_ARGS__)

/**
 * LOG_PRINT() -- Log print macro.
 */
#define LOG_PRINT(loglevel, module, str) fprintf(stderr, module ": " str "\n");


/* Some convenience macros */
#define LOG_PRINT_ERROR(module, str) LOG_PRINT(MB_LOGLEVEL_ERROR, module, str)
#define LOG_VPRINT_ERROR(module, fmt, ...) LOG_VPRINT(MB_LOGLEVEL_ERROR, module, fmt, __VA_ARGS__)

#endif
