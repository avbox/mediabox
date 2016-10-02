#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <sys/time.h>

static const struct timespec zerotime = { .tv_sec = 0, .tv_nsec = 0 };


static inline int
_timelt(const struct timespec *time1, const struct timespec *time2)
{
	if (time1->tv_sec < time2->tv_sec) {
		return 1;
	} else if (time1->tv_sec > time2->tv_sec) {
		return 0;
	} else if (time1->tv_nsec < time2->tv_nsec) {
		return 1;
	} else {
		return 0;
	}
}


int
timelt(const struct timespec *time1, const struct timespec *time2)
{
	return _timelt(time1, time2);
}


int
timelte(const struct timespec *time1, const struct timespec *time2)
{
	if (time1->tv_sec == time2->tv_sec && time1->tv_nsec == time2->tv_nsec) {
		return 1;
	} else {
		return _timelt(time1, time2);
	}
}


struct timespec
timeadd(const struct timespec *time1, const struct timespec *time2)
{
	struct timespec tmp;
	int64_t nsec = time1->tv_nsec + time2->tv_nsec;

	tmp.tv_sec = time1->tv_sec + time2->tv_sec;

	while (nsec > 1000L * 1000L * 1000L) {
		nsec -= 1000L * 1000L * 1000L;
		tmp.tv_sec++;
	}

	tmp.tv_nsec = nsec;
	return tmp;
}


struct timespec
timediff(const struct timespec *start, const struct timespec *end)
{
	struct timespec temp;

	if (start == NULL) {
		start = &zerotime;
	}
	if (end == NULL) {
		end = &zerotime;
	}

	if ((end->tv_nsec - start->tv_nsec)<0) {
		temp.tv_sec = end->tv_sec - start->tv_sec - 1;
		temp.tv_nsec = 1000000000 + end->tv_nsec - start->tv_nsec;
	} else {
		temp.tv_sec = end->tv_sec - start->tv_sec;
		temp.tv_nsec = end->tv_nsec - start->tv_nsec;
	}
	return temp;
}


/**
 * Gets the absolute time.
 * This is useful for pthread_cond_timedwait().
 */
struct timespec *
abstime(struct timespec * const tv)
{
	struct timeval now; 
	gettimeofday(&now, NULL);
	long int abstime_ns_large = now.tv_usec * 1000L;
	tv->tv_sec = now.tv_sec + (abstime_ns_large / 1000000000L);
	tv->tv_nsec = abstime_ns_large % 1000000000L;
	return tv;
}


/**
 * Add the absolute time (returned by abstime()) to the timespec
 * structure argument.
 */
struct timespec *
delay2abstime(struct timespec * const tv)
{
	struct timespec abs;
	abstime(&abs);
	*tv = timeadd(tv, &abs);
	return tv;
}



int64_t
utimediff(const struct timespec *a, const struct timespec *b)
{
	if (a == NULL) {
		a = &zerotime;
	}
	if (b == NULL) {
		b = &zerotime;
	}
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



