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


#ifdef HAVE_CONFIG_H
#	include "../config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <features.h>
#include <pthread.h>
#include <sys/time.h>

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif


static FILE *logfile = NULL;
static pthread_mutex_t iolock = PTHREAD_MUTEX_INITIALIZER;


void
log_setfile(FILE * const f)
{
	assert(f != NULL);
	logfile = f;
}


size_t
log_printf(const char * const fmt, ...)
{
	size_t ret;
	va_list args;
	struct timespec tv;

	(void) clock_gettime(CLOCK_MONOTONIC, &tv);

	va_start(args, fmt);
	pthread_mutex_lock(&iolock);
	fprintf(logfile, "[%08li.%09li] ", tv.tv_sec, tv.tv_nsec);
	ret = vfprintf(logfile, fmt, args);
	fflush(logfile);
	pthread_mutex_unlock(&iolock);
	va_end(args);
	return ret;
}


void
log_backtrace(void)
{
#ifdef HAVE_BACKTRACE
	void *bt[20];
	size_t sz;
	char **strings;

	sz = backtrace(bt, 20);
	if ((strings = backtrace_symbols(bt, sz)) == NULL) {
		return;
	}

	for (size_t i = 0; i < sz; i++) {
		log_printf("%s", strings[i]);
	}
	free(strings);
#endif
}


void
log_init()
{
	logfile = stderr;
}
