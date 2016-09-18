#ifndef __MB_DEBUG_H__
#define __MB_DEBUG_H__


#include <stdio.h>


/**
 * MB_DEBUG_SET_THREAD_NAME() -- Set the thread name as seen in debugger.
 */
#if !defined(NDEBUG) && defined(_GNU_SOURCE)
#define MB_DEBUG_SET_THREAD_NAME(name) pthread_setname_np(pthread_self(), name)
#else
#define MB_DEBUG_SET_THREAD_NAME(name) (void) name
#endif

/**
 * DEBUG_VPRINT() - Variadic debug print macro.
 */
#ifndef NDEBUG
#define DEBUG_VPRINT(module, fmt, ...) fprintf(stderr, module ": " fmt "\n", __VA_ARGS__)
#else
#define DEBUG_VPRINT(module, fmt, ...) (void) 0
#endif

/**
 * DEBUG_PRINT() -- Debug print macro.
 */
#ifndef NDEBUG
#define DEBUG_PRINT(module, str) fprintf(stderr, module ": " str "\n");
#else
#define DEBUG_PRINT(module, str) (void) 0
#endif

#endif
