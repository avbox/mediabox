#ifndef __MB_AUDIO_H__
#define __MB_AUDIO_H__

#include <stdint.h>

/**
 * Opaque stream structure
 */
struct avbox_audiostream;


/**
 * Stream dried callback.
 */
typedef void (*avbox_audiostream_underrun_callback)(
	struct avbox_audiostream * const inst, void * const context);


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
	const uint8_t * const data, const size_t n_frames);


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
 * Starts the stream playback.
 */
int
avbox_audiostream_start(struct avbox_audiostream * const stream);


/**
 * Set the audio clock offset.
 */
int
avbox_audiostream_setclock(struct avbox_audiostream * const stream, const int64_t clock);


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
	avbox_audiostream_underrun_callback underrun_callback,
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
