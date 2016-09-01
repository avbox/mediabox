#ifndef __TIME_UTIL_H__
#define __TIME_UTIL_H__

#include <stdint.h>


int
timelt(const struct timespec *time1, const struct timespec *time2);


int
timelte(const struct timespec *time1, const struct timespec *time2);


struct timespec
timeadd(const struct timespec *time1, const struct timespec *time2);


struct timespec
abstime(void);


struct timespec
timediff(const struct timespec *start, const struct timespec *end);


int64_t
utimediff(const struct timespec *a, const struct timespec *b);


void
utimeadd(struct timespec *t, unsigned int usecs);


#endif
