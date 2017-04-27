/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>


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
log_init()
{
	logfile = stderr;
}
