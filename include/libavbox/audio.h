#ifndef __MB_AUDIO_H__
#define __MB_AUDIO_H__

#include <stdint.h>


#define AVBOX_AUDIOSTREAM_UNDERRUN		(1)
#define AVBOX_AUDIOSTREAM_CRITICAL_ERROR	(2)
#define AVBOX_AUDIOSTREAM_PACKET_RELEASED	(3)


/**
 * Opaque stream structure
 */
struct avbox_audiostream;


/**
 * Check if the stream is blocking another thread on write().
 */
int
avbox_audiostream_blocking(struct avbox_audiostream * const stream);


/**
 * Stream dried callback.
 */
typedef void (*avbox_audiostream_callback)(
	struct avbox_audiostream * const inst, int msg, void * const data, void * const context);


/**
 * Sends a clock packet to the audio stream.
 */
int
avbox_audiostream_setclock(struct avbox_audiostream * const inst,
	const int64_t time);


/**
 * Flush an audio stream.
 */
void
avbox_audiostream_drop(struct avbox_audiostream * const inst);


/**
 * Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
int64_t
avbox_audiostream_gettime(struct avbox_audiostream * const inst);


/**
 * Pauses the audio stream and synchronizes
 * the audio clock.
 */
int
avbox_audiostream_pause(struct avbox_audiostream * const inst);


/**
 * Resume audio playback
 */
int
avbox_audiostream_resume(struct avbox_audiostream * const inst);


/**
 * Writes n_frames audio frames to the stream.
 */
int
avbox_audiostream_write(struct avbox_audiostream * const stream,
	uint8_t * const data, const size_t n_frames, void * const callback_handle);


/**
 * Check if the audio stream is paused.
 */
int
avbox_audiostream_ispaused(const struct avbox_audiostream * const stream);


/**
 * Get the number of frames buffered.
 */
unsigned int
avbox_audiostream_count(struct avbox_audiostream * const stream);


/**
 * Get the number of frames buffered.
 */
unsigned int
avbox_audiostream_size(struct avbox_audiostream * const stream);


/**
 * Starts the stream playback.
 */
int
avbox_audiostream_start(struct avbox_audiostream * const stream);


/**
 * Waits for the buffer to be empty and pauses
 * the stream.
 */
int
avbox_audiostream_drain(struct avbox_audiostream * const inst);


/**
 * Create a new sound stream.
 */
struct avbox_audiostream *
avbox_audiostream_new(int max_frames,
	avbox_audiostream_callback underrun_callback,
	void * const callback_context);


/**
 * Destroy a sound stream.
 */
void
avbox_audiostream_destroy(struct avbox_audiostream * const stream);


/**
 * Initialize audio subsystem.
 */
int
avbox_audiostream_init(void);


/**
 * Shutdown audio subsystem.
 */
void
avbox_audiostream_shutdown(void);


#endif
