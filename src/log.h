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

#endif
