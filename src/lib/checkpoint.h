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


#define AVBOX_CHECKPOINT_READY		(0)
#define AVBOX_CHECKPOINT_HALTING	(1)
#define AVBOX_CHECKPOINT_HALTED		(2)
#define AVBOX_CHECKPOINT_DISABLED	(3)


struct avbox_checkpoint
{
	int state;
	pthread_mutex_t mutex;
	pthread_cond_t halted;
	pthread_cond_t released;
};

typedef struct avbox_checkpoint avbox_checkpoint_t;


static inline void
avbox_checkpoint_init(avbox_checkpoint_t * const checkpoint)
{
	if (pthread_mutex_init(&checkpoint->mutex, NULL) != 0 ||
		pthread_cond_init(&checkpoint->halted, NULL) != 0 ||
		pthread_cond_init(&checkpoint->released, NULL) != 0) {
		abort();
	}
	checkpoint->state = AVBOX_CHECKPOINT_DISABLED;
}

static inline void
avbox_checkpoint_here(avbox_checkpoint_t * const checkpoint)
{
	assert(checkpoint->state != AVBOX_CHECKPOINT_DISABLED);
	pthread_mutex_lock(&checkpoint->mutex);
	if (checkpoint->state == AVBOX_CHECKPOINT_HALTING) {
		checkpoint->state = AVBOX_CHECKPOINT_HALTED;
		pthread_cond_signal(&checkpoint->halted);
		pthread_cond_wait(&checkpoint->released, &checkpoint->mutex);
		checkpoint->state = AVBOX_CHECKPOINT_READY;
	}
	pthread_mutex_unlock(&checkpoint->mutex);
}


static inline void
avbox_checkpoint_disable(avbox_checkpoint_t * const checkpoint)
{
	pthread_mutex_lock(&checkpoint->mutex);
	checkpoint->state = AVBOX_CHECKPOINT_DISABLED;
	pthread_cond_signal(&checkpoint->halted);
	pthread_mutex_unlock(&checkpoint->mutex);
}


static inline void
avbox_checkpoint_enable(avbox_checkpoint_t * const checkpoint)
{
	pthread_mutex_lock(&checkpoint->mutex);
	checkpoint->state = AVBOX_CHECKPOINT_READY;
	pthread_cond_signal(&checkpoint->halted);
	pthread_mutex_unlock(&checkpoint->mutex);
}


static inline void
avbox_checkpoint_halt(avbox_checkpoint_t * const checkpoint)
{
	assert(checkpoint->state == AVBOX_CHECKPOINT_READY);
	pthread_mutex_lock(&checkpoint->mutex);
	if (checkpoint->state != AVBOX_CHECKPOINT_DISABLED) {
		checkpoint->state = AVBOX_CHECKPOINT_HALTING;
	}
	pthread_mutex_unlock(&checkpoint->mutex);

}

static inline void
avbox_checkpoint_wait(avbox_checkpoint_t * const checkpoint)
{
	while (checkpoint->state != AVBOX_CHECKPOINT_HALTED &&
		checkpoint->state != AVBOX_CHECKPOINT_DISABLED) {
		pthread_mutex_lock(&checkpoint->mutex);
		if (checkpoint->state != AVBOX_CHECKPOINT_HALTED) {
			pthread_cond_wait(&checkpoint->halted, &checkpoint->mutex);
		}
		pthread_mutex_unlock(&checkpoint->mutex);
	}
}


static inline void
avbox_checkpoint_continue(avbox_checkpoint_t * const checkpoint)
{
	pthread_mutex_lock(&checkpoint->mutex);
	pthread_cond_signal(&checkpoint->released);
	pthread_mutex_unlock(&checkpoint->mutex);
}

#endif
