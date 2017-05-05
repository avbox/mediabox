/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __TIME_UTIL_H__
#define __TIME_UTIL_H__

#include <stdint.h>


/*
 * Time conversion macros
 */
#define SEC2MSEC(sec)	((sec) * 1000L)
#define SEC2USEC(sec)	((sec) * 1000L * 1000L)
#define SEC2NSEC(sec)	((sec) * 1000L * 1000L * 1000L)
#define MSEC2USEC(msec)	((msec) * (1000L))
#define MSEC2SEC(msec)	((msec) * (1000L * 1000L))
#define MSEC2NSEC(msec)	((msec) / (1000L))
#define NSEC2USEC(nsec)	((nsec) / (1000L))
#define NSEC2MSEC(nsec)	((nsec) / (1000L * 1000L))
#define NSEC2SEC(nsec)	((nsec) / (1000L * 1000L * 1000L))



int
timelt(const struct timespec *time1, const struct timespec *time2);


int
timelte(const struct timespec *time1, const struct timespec *time2);


/**
 * Adds two timespec structures.
 */
struct timespec
timeadd(const struct timespec * const time1, const struct timespec * const time2);


/**
 * Gets the absolute time.
 * This is useful for pthread_cond_timedwait().
 */
struct timespec *
abstime(struct timespec * const tv);



struct timespec
timediff(const struct timespec *start, const struct timespec *end);


int64_t
utimediff(const struct timespec *a, const struct timespec *b);


/**
 * Adds the specified number of micro-seconds to the timespec
 * struct.
 */
void
timeaddu(struct timespec * const tv, const int64_t usecs);


/**
 * Add the absolute time (returned by abstime()) to the timespec
 * structure argument.
 */
struct timespec *
delay2abstime(struct timespec * const tv);


#endif
