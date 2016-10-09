#ifndef __MB_AUDIO_H__
#define __MB_AUDIO_H__


/**
 * Opaque stream structure
 */
struct mb_audio_stream;


/**
 * Flush an audio stream.
 */
void
mb_audio_stream_drop(struct mb_audio_stream * const inst);


/**
 * Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
int64_t
mb_audio_stream_gettime(struct mb_audio_stream * const inst);


/**
 * Pauses the audio stream and synchronizes
 * the audio clock.
 */
int
mb_audio_stream_pause(struct mb_audio_stream * const inst);


/**
 * Resume audio playback
 */
void
mb_audio_stream_resume(struct mb_audio_stream * const inst);


/**
 * Writes n_frames audio frames to the stream.
 */
int
mb_audio_stream_write(struct mb_audio_stream * const stream,
	const uint8_t * const data, const size_t n_frames);


/**
 * Check if the audio stream is paused.
 */
int
mb_audio_stream_ispaused(const struct mb_audio_stream * const stream);


/**
 * Get the number of frames buffered.
 */
unsigned int
mb_audio_stream_getframecount(struct mb_audio_stream * const stream);


/**
 * Starts the stream playback.
 */
int
mb_audio_stream_start(struct mb_audio_stream * const stream);


/**
 * Set the audio clock offset.
 */
int
mb_audio_stream_setclock(struct mb_audio_stream * const stream, const int64_t clock);


/**
 * Create a new sound stream.
 */
struct mb_audio_stream *
mb_audio_stream_new(void);


/**
 * Destroy a sound stream.
 */
void
mb_audio_stream_destroy(struct mb_audio_stream * const stream);

#endif
