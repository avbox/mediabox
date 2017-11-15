#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

#define LOG_MODULE "stopwatch"

#include "log.h"
#include "debug.h"
#include "time_util.h"

#define CLOCK_ID	(CLOCK_MONOTONIC_COARSE)

struct avbox_stopwatch
{
	int running;
	int64_t reset;	/*< time when the stopwatch was reset */
	int64_t value;  /*< the value the stopwatch was reset to */
};


const struct timespec zero = { .tv_sec = 0, .tv_nsec = 0 };


/**
 * Sets the stopwatch and stops it.
 */
void
avbox_stopwatch_reset(struct avbox_stopwatch * const inst, int64_t value)
{
	struct timespec now;
	clock_gettime(CLOCK_ID, &now);
	inst->reset = utimediff(&now, &zero);
	inst->value = value;
	inst->running = 0;
}


/**
 * Get the current stopwatch time
 */
int64_t
avbox_stopwatch_time(const struct avbox_stopwatch * const inst)
{
	if (inst->running) {
		struct timespec now;
		clock_gettime(CLOCK_ID, &now);
		return (utimediff(&now, &zero) - inst->reset) + inst->value;
	} else {
		return inst->value;
	}
}


/**
 * Start the stopwatch.
 */
void
avbox_stopwatch_start(struct avbox_stopwatch * const inst)
{
	ASSERT(inst != NULL);
	ASSERT(!inst->running);
	avbox_stopwatch_reset(inst,
		avbox_stopwatch_time(inst));
	inst->running = 1;
}


/**
 * Stops the stopwatch.
 */
void
avbox_stopwatch_stop(struct avbox_stopwatch * const inst)
{
	ASSERT(inst != NULL);
	ASSERT(inst->running);
	avbox_stopwatch_reset(inst,
		avbox_stopwatch_time(inst));
}


/**
 * Create new stopwatch.
 */
struct avbox_stopwatch *
avbox_stopwatch_new()
{
	struct avbox_stopwatch *inst;

	if ((inst = malloc(sizeof(struct avbox_stopwatch))) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}
	memset(inst, 0, sizeof(struct avbox_stopwatch));
	return inst;
}


/**
 * Free all resources used by stopwatch.
 */
void
avbox_stopwatch_destroy(struct avbox_stopwatch * const inst)
{
	free(inst);
}
