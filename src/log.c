/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <pthread.h>


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
	va_start(args, fmt);
	pthread_mutex_lock(&iolock);
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
