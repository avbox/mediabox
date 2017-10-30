/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <sys/time.h>

#include "compiler.h"

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
	if (UNLIKELY(time1->tv_sec == time2->tv_sec && time1->tv_nsec == time2->tv_nsec)) {
		return 1;
	} else {
		return _timelt(time1, time2);
	}
}


int
timegt(const struct timespec *time1, const struct timespec *time2)
{
	return !timelte(time1, time2);
}


int
timegte(const struct timespec *time1, const struct timespec *time2)
{
	return !timelt(time1, time2);
}


int
timeeq(const struct timespec *time1, const struct timespec *time2)
{
	if (time1->tv_nsec != time2->tv_nsec || time1->tv_sec != time2->tv_sec) {
		return 0;
	}
	return 1;
}

/**
 * Adds two timespec structures.
 */
struct timespec
timeadd(const struct timespec * const time1, const struct timespec * const time2)
{
	struct timespec tmp;
	const int64_t nsec = time1->tv_nsec + time2->tv_nsec;
	tmp.tv_sec = time1->tv_sec + time2->tv_sec;
	tmp.tv_sec += nsec / (1000L * 1000L * 1000L);
	tmp.tv_nsec = nsec % (1000L * 1000L * 1000L);
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
		temp.tv_nsec = 1000000000L + end->tv_nsec - start->tv_nsec;
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


/**
 * Calculates the time difference between the arguments
 * in micro-seconds.
 */
int64_t
utimediff(const struct timespec * a, const struct timespec * b)
{

	if (UNLIKELY(a == NULL)) {
		a = &zerotime;
	}
	if (UNLIKELY(b == NULL)) {
		b = &zerotime;
	}
	const int64_t aa = ((a->tv_sec * 1000L * 1000L * 1000L) + a->tv_nsec) / 1000L;
	const int64_t bb = ((b->tv_sec * 1000L * 1000L * 1000L) + b->tv_nsec) / 1000L;
	return (aa - bb);
}


/**
 * Adds the specified number of micro-seconds to the timespec
 * struct.
 */
void
timeaddu(struct timespec * const tv, const int64_t usecs)
{
	tv->tv_sec += usecs / (1000L * 1000L);
	tv->tv_nsec += (usecs % (1000L * 1000L)) * 1000L;;
}

