#ifndef __TIME_UTIL_H__
#define __TIME_UTIL_H__

#include <stdint.h>

struct timespec
timediff(const struct timespec *start, const struct timespec *end);


int64_t
utimediff(const struct timespec *a, const struct timespec *b);


void
utimeadd(struct timespec *t, unsigned int usecs);


#endif
