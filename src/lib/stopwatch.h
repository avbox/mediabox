#ifndef __STOPWATCH_H__
#define __STOPWATCH_H__

#include <stdint.h>


struct avbox_stopwatch;


int
avbox_stopwatch_running(const struct avbox_stopwatch * const inst);


/**
 * Sets the stopwatch and stops it.
 */
void
avbox_stopwatch_reset(struct avbox_stopwatch * const inst, int64_t value);


/**
 * Get the current stopwatch time
 */
int64_t
avbox_stopwatch_time(const struct avbox_stopwatch * const inst);


/**
 * Start the stopwatch.
 */
void
avbox_stopwatch_start(struct avbox_stopwatch * const inst);


/**
 * Stops the stopwatch.
 */
void
avbox_stopwatch_stop(struct avbox_stopwatch * const inst);


/**
 * Create new stopwatch.
 */
struct avbox_stopwatch *
avbox_stopwatch_new();


/**
 * Free all resources used by stopwatch.
 */
void
avbox_stopwatch_destroy(struct avbox_stopwatch * const inst);

#endif
