#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <inttypes.h>

#define LOG_MODULE "stopwatch"

#include "log.h"
#include "debug.h"
#include "time_util.h"
#include "stopwatch.h"

#define CLOCK_ID	(CLOCK_MONOTONIC_COARSE)

struct avbox_stopwatch
{
	int running;
	int64_t reset;	/*< time when the stopwatch was reset */
	int64_t value;  /*< the value the stopwatch was reset to */
};


/**
 * Sets the stopwatch and stops it.
 */
void
avbox_stopwatch_reset(struct avbox_stopwatch * const inst, int64_t value)
{
	struct timespec now;
	DEBUG_VPRINT(LOG_MODULE, "Resseting stopwath to %"PRIi64,
		value);

	clock_gettime(CLOCK_ID, &now);
	ASSERT(now.tv_sec >= 0 && now.tv_nsec >= 0);
	inst->reset = utimediff(&now, NULL);
	inst->value = value;
	inst->running = 0;

	DEBUG_VPRINT(LOG_MODULE, "After reset inst->value=%"PRIi64" value=%"PRIi64" time=%"PRIi64,
		inst->value, value, avbox_stopwatch_time(inst));
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
		const int64_t ret = (utimediff(&now, NULL) - inst->reset) + inst->value;
		/* DEBUG_VPRINT(LOG_MODULE, "Returning %"PRIi64" now_s=%"PRIi64" now_ns=%"PRIi64" reset=%"PRIi64" value=%"PRIi64,
			ret, (int64_t) now.tv_sec, (int64_t) now.tv_nsec, inst->reset, inst->value); */
		return ret;
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


int
avbox_stopwatch_running(const struct avbox_stopwatch * const inst)
{
	return inst->running;
}


/**
 * Stops the stopwatch.
 */
void
avbox_stopwatch_stop(struct avbox_stopwatch * const inst)
{
	ASSERT(inst != NULL);
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
