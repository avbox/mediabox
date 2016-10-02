#ifndef __TIME_UTIL_H__
#define __TIME_UTIL_H__

#include <stdint.h>


int
timelt(const struct timespec *time1, const struct timespec *time2);


int
timelte(const struct timespec *time1, const struct timespec *time2);


struct timespec
timeadd(const struct timespec *time1, const struct timespec *time2);


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


void
utimeadd(struct timespec *t, unsigned int usecs);


/**
 * Add the absolute time (returned by abstime()) to the timespec
 * structure argument.
 */
struct timespec *
delay2abstime(struct timespec * const tv);


#endif
