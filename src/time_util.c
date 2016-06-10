#include <stdlib.h>
#include <time.h>
#include <stdint.h>

struct timespec
timediff(struct timespec *start, struct timespec *end)
{
	struct timespec temp;
	if ((end->tv_nsec - start->tv_nsec)<0) {
		temp.tv_sec = end->tv_sec - start->tv_sec - 1;
		temp.tv_nsec = 1000000000 + end->tv_nsec - start->tv_nsec;
	} else {
		temp.tv_sec = end->tv_sec - start->tv_sec;
		temp.tv_nsec = end->tv_nsec - start->tv_nsec;
	}
	return temp;
}

int64_t
utimediff(struct timespec *a, struct timespec *b)
{
	int64_t aa = ((a->tv_sec * 1000 * 1000 * 1000) + a->tv_nsec) / 1000;
	int64_t bb = ((b->tv_sec * 1000 * 1000 * 1000) + b->tv_nsec) / 1000;
	int64_t cc = aa - bb;
	return cc;
}

void
utimeadd(struct timespec *t, unsigned int usecs)
{
	uint64_t nsecs;

	nsecs = t->tv_nsec + (usecs * 1000);
	while (nsecs > (1000 * 1000 * 1000)) {
		nsecs -= 1000 * 1000 * 1000;
		t->tv_sec++;
	}
	t->tv_nsec = nsecs;
}



