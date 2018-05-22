#ifndef __TORRENT_STREAM_H__
#define __TORRENT_STREAM_H__


#define AVBOX_TORRENTFLAGS_NONE		(0)
#define AVBOX_TORRENTFLAGS_HIDDEN	(1)
#define AVBOX_TORRENTFLAGS_STREAM	(2)
#define AVBOX_TORRENTFLAGS_AUTOCLOSE	(4)


struct avbox_torrent;


/**
 * Close a torrent stream.
 */
EXPORT void
avbox_torrent_close(struct avbox_torrent * const inst);


/**
 * Read from a torrent stream.
 */
EXPORT int
avbox_torrent_read(struct avbox_torrent * const inst,
	uint8_t *buf, int sz);


EXPORT int
avbox_torrent_seek(struct avbox_torrent * const inst,
	const int64_t pos);


EXPORT int64_t
avbox_torrent_tell(const struct avbox_torrent * const inst);


EXPORT int64_t
avbox_torrent_size(const struct avbox_torrent * const inst);


EXPORT void
avbox_torrent_bufferstate(struct avbox_torrent * const inst,
	int64_t * const count, int64_t * const capacity);


EXPORT int64_t
avbox_torrent_downloaded(const struct avbox_torrent * const inst);


EXPORT const char *
avbox_torrent_name(const struct avbox_torrent * const inst);


EXPORT const char *
avbox_torrent_id(const struct avbox_torrent * const inst);


/**
 * Move the torrent to the specified location when
 * finished and then close the stream
 */
EXPORT void
avbox_torrent_moveonfinish(struct avbox_torrent * const inst,
	const char * const dest);


/**
 * Open a torrent stream stream.
 */
EXPORT struct avbox_torrent*
avbox_torrent_open(const char * const uri, const char * const file,
	unsigned int flags, struct avbox_object *notify_object);


/**
 * Get a reference the next active torrent.
 */
EXPORT struct avbox_torrent*
avbox_torrent_next(struct avbox_torrent *current);


/**
 * Release a reference to a torrent object.
 */
EXPORT void
avbox_torrent_unref(struct avbox_torrent * const inst);

int
avbox_torrent_init(void);


void
avbox_torrent_shutdown(void);

#endif
