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


#ifndef __AVBOX_CHECKPOINT__
#define __AVBOX_CHECKPOINT__

#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

#include "debug.h"
#include "compiler.h"
#include "time_util.h"

#define AVBOX_CHECKPOINT_ENABLED	(0x1)
#define AVBOX_CHECKPOINT_HALTED 	(0x2) 


struct avbox_checkpoint
{
	int count;
	int state;
	pthread_mutex_t mutex;
	pthread_cond_t halted;
	pthread_cond_t released;
};


typedef struct avbox_checkpoint avbox_checkpoint_t;


static inline void
avbox_checkpoint_here(avbox_checkpoint_t * const checkpoint)
{
	ASSERT(checkpoint->state & AVBOX_CHECKPOINT_ENABLED);

	if (checkpoint->count > 0 ) {
		while (checkpoint->count > 0) {
			pthread_mutex_lock(&checkpoint->mutex);
			if (checkpoint->count > 0 ) {
				checkpoint->state |= AVBOX_CHECKPOINT_HALTED;
				pthread_cond_signal(&checkpoint->halted);
				pthread_cond_wait(&checkpoint->released, &checkpoint->mutex);
			}
			pthread_mutex_unlock(&checkpoint->mutex);
		}
		checkpoint->state &= ~AVBOX_CHECKPOINT_HALTED;
	}
}


static inline void
avbox_checkpoint_disable(avbox_checkpoint_t * const checkpoint)
{
	pthread_mutex_lock(&checkpoint->mutex);
	checkpoint->state &= ~AVBOX_CHECKPOINT_ENABLED;
	pthread_cond_signal(&checkpoint->halted);
	pthread_mutex_unlock(&checkpoint->mutex);
}


static inline void
avbox_checkpoint_enable(avbox_checkpoint_t * const checkpoint)
{
	pthread_mutex_lock(&checkpoint->mutex);
	checkpoint->state |= AVBOX_CHECKPOINT_ENABLED;
	pthread_cond_broadcast(&checkpoint->halted);
	pthread_mutex_unlock(&checkpoint->mutex);
}


static inline void
avbox_checkpoint_halt(avbox_checkpoint_t * const checkpoint)
{
	pthread_mutex_lock(&checkpoint->mutex);
	checkpoint->count++;
	pthread_mutex_unlock(&checkpoint->mutex);

}


static inline int
avbox_checkpoint_wait(avbox_checkpoint_t * const checkpoint, int64_t timeout)
{
	int ret = 0;

	/* block while we are enabled but not halted */
	pthread_mutex_lock(&checkpoint->mutex);
	if (checkpoint->state == AVBOX_CHECKPOINT_ENABLED) {
		struct timespec tv;
		tv.tv_sec = 0;
		tv.tv_nsec = timeout;
		delay2abstime(&tv);
		pthread_cond_timedwait(&checkpoint->halted, &checkpoint->mutex, &tv);
		if (checkpoint->state != AVBOX_CHECKPOINT_ENABLED) {
			ret = 1;
		}
	} else {
		ret = 1;
	}
	pthread_mutex_unlock(&checkpoint->mutex);
	return ret;
}


static inline void
avbox_checkpoint_continue(avbox_checkpoint_t * const checkpoint)
{
	pthread_mutex_lock(&checkpoint->mutex);
	ASSERT(checkpoint->count > 0);
	checkpoint->count--;
	pthread_cond_signal(&checkpoint->released);
	pthread_mutex_unlock(&checkpoint->mutex);
}


static inline int
avbox_checkpoint_halted(avbox_checkpoint_t * const checkpoint)
{
	return (checkpoint->state & AVBOX_CHECKPOINT_HALTED) != 0;
}


static inline void
avbox_checkpoint_init(avbox_checkpoint_t * const checkpoint)
{
	if (pthread_mutex_init(&checkpoint->mutex, NULL) != 0 ||
		pthread_cond_init(&checkpoint->halted, NULL) != 0 ||
		pthread_cond_init(&checkpoint->released, NULL) != 0) {
		abort();
	}
	checkpoint->count = 0;
	checkpoint->state = 0;
}

#endif
