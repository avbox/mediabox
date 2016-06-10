#ifndef __TIME_UTIL_H__
#define __TIME_UTIL_H__

#include <stdint.h>

struct timespec
timediff(struct timespec *start, struct timespec *end);


int64_t
utimediff(struct timespec *a, struct timespec *b);


void
utimeadd(struct timespec *t, unsigned int usecs);


#endif
