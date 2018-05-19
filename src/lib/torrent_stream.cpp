#ifdef HAVE_CONFIG_H
#	include "../config.h"
#endif

#include <queue>
#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/extensions.hpp>


#define LOG_MODULE "torrent_stream"


extern "C" {
	#include <inttypes.h>
	#include <sys/types.h>
	#include <sys/stat.h>
	#include <unistd.h>
	#include <fcntl.h>
	#include <pthread.h>
	#include "avbox.h"
}


#define STRINGIZE2(x)   #x
#define STRINGIZE(x)    STRINGIZE2(x)

#define READAHEAD_TAIL	(1024 * 1024 * 5)	/* bytes to read from end of file during warmup */
#define READAHEAD_MIN	(1024 * 1024 * 15)	/* bytes to try to keep on readahead */

#define AVBOX_TORRENTMSG_METADATA_RECEIVED	(AVBOX_MESSAGETYPE_USER)


namespace lt = libtorrent;


struct alerts_observer_plugin : lt::plugin
{
	alerts_observer_plugin() {}
	virtual boost::uint32_t implemented_features() { return lt::plugin::reliable_alerts_feature; }
	virtual void on_alert(lt::alert const* a);
};


struct piece_header
{
	boost::shared_array<char> buffer;
	int size;
	int index;
};


struct piece_status
{
	int blocks_finished:15;
	int check_passed:1;
	int ready:1;
};


typedef std::queue<boost::shared_ptr<struct piece_header> > piece_queue_t;


LISTABLE_STRUCT(avbox_torrent,
	int64_t file_offset;	/* the offset of the file that we're streaming */
	int64_t filesize;	/* the size of the file we're streaming */
	int64_t pos;		/* the stream position */
	int64_t ra_pos;		/* the current readahead position */
	int64_t torrent_size;	/* the size all torrent files in bytes */
	int n_pieces;		/* the number of pieces in the torrent */
	int piece_size;		/* the size of each piece */
	int last_piece_size;	/* the size of the last piece. (To avoid 64bit division) */
	int next_piece;		/* the next piece that the readahead thread is waiting for */
	int blocks_per_piece;	/* the number of blocks per piece */
	int block_size;		/* the block size */
	int have_metadata;	/* all the fields above are valid if set to 1 */

	int closed;				/* object closed and being destroyed */
	int user_waiting;			/* this is non-zero while the user thread is blocked */
	int readahead_min;			/* the number of bytes to keep on readahead. For now this is static */
	int underrun;				/* underrun flag. */
	int warmed;				/* this flag is set to true after the stream has warmed up */
	int n_avail_pieces;			/* the number of pieces downloaded */
	int bitrate;				/* bitrate hint */
	unsigned int flags;			/* flags */

	pthread_cond_t readahead_cond;		/* used for waking the readahead thread */
	pthread_cond_t user_cond;		/* used for waking the user thread */
	std::vector<piece_status> avail_pieces;	/* list of downloaded pieces */
	piece_queue_t readahead_pieces;		/* the readahead queue */
	struct avbox_thread *readahead_thread;	/* the readahead thread */
	struct avbox_delegate *readahead_fn;	/* the readahead worker */
	struct avbox_object *object;		/* our own object */
	struct avbox_object *notify_object;	/* object to send notifications to */
	lt::torrent_handle handle;		/* torrent handle */
	std::string name;			/* torrent name */
	std::string info_hash;			/* info hash */
	std::string filename;			/* the filename that we're streaming */
	std::string files_path;			/* the temporary storage path */
	std::string move_to;			/* the path where the files will be moved to */
	pthread_mutex_t lock;			/* protects this structure */
);


static int quit = 0;
static lt::session *session = nullptr;
static LIST torrents;
static pthread_mutex_t session_lock;

static const std::string storage_path(STRINGIZE(LOCALSTATEDIR) "/lib/mediabox/store/downloads");
static const std::string torrents_path(std::string(STRINGIZE(LOCALSTATEDIR)) + "/lib/mediabox/torrents/");


static int
offset_to_piece_index(const struct avbox_torrent * const inst, int64_t offset)
{
	ASSERT(inst->have_metadata);
	/* ASSERT(offset < inst->filesize); */
	return (offset + inst->file_offset) / inst->piece_size;
}


static int
piece_size(const struct avbox_torrent * const inst, const int index)
{
	ASSERT(inst->have_metadata);
	ASSERT(index >= 0 && index < inst->n_pieces);
	if (index < inst->n_pieces - 1) {
		return inst->piece_size;
	} else {
		return inst->last_piece_size;
	}
}


static int
blocks_in_piece(const struct avbox_torrent * const inst, const int index)
{
	ASSERT(inst->have_metadata);
	ASSERT(index >= 0 && index < inst->n_pieces);

	if (index < inst->n_pieces - 1) {
		return inst->blocks_per_piece;
	} else {
		return (piece_size(inst, index) + inst->block_size - 1) / inst->block_size;
	}
}


static struct piece_status&
get_piece_status(struct avbox_torrent * const inst, const int index)
{
	if (inst->avail_pieces.size() <= (unsigned int) index) {
		inst->avail_pieces.resize(index + 1, { 0, 0 });
	}
	return inst->avail_pieces[index];
}


static int
have_piece(const struct avbox_torrent * const inst, const int index)
{
	ASSERT(inst->have_metadata);
	ASSERT(index >= 0 && index < inst->n_pieces);

	if (inst->avail_pieces.size() <= (unsigned int) index) {
		return 0;
	}

	return inst->avail_pieces[index].ready;
}


static void
cleanup_temp_directory()
{
	DEBUG_PRINT(LOG_MODULE, "Cleaning up temp directory");
}


/**
 * Gets the number of bytes that are already available
 * on disk starting from the current stream position.
 */
static int64_t
get_torrent_pos(const struct avbox_torrent * const inst)
{
	ASSERT(inst->have_metadata);
	ASSERT(inst->flags & AVBOX_TORRENTFLAGS_STREAM);

	int64_t pos = inst->pos;
	int piece_index = offset_to_piece_index(inst, inst->pos);
	while (piece_index < inst->n_pieces && have_piece(inst, piece_index)) {
		pos += piece_size(inst, piece_index++);
	}
	return pos;
}


static void
torrent_finished(struct avbox_torrent * const inst)
{
	ASSERT(inst->have_metadata);

	bool finished = true;
	for (int i = 0; i < inst->n_pieces; i++) {
		if (!have_piece(inst, i)) {
			DEBUG_VPRINT(LOG_MODULE, "Piece %i missing. Not done!",
				i);
			finished = false;
			break;
		}
	}
	if (finished) {
		DEBUG_PRINT(LOG_MODULE, "We really are done");
		if (inst->move_to.empty()) {
			if (inst->flags & AVBOX_TORRENTFLAGS_AUTOCLOSE) {
				avbox_torrent_close(inst);
			}
		} else {
			ASSERT(inst->handle.is_valid());
			inst->handle.move_storage(inst->move_to,
				lt::always_replace_files);
			DEBUG_VPRINT(LOG_MODULE, "Storage move requested for %s",
				inst->info_hash.c_str());
		}
	} else {
		DEBUG_PRINT(LOG_MODULE, "Not really done");
	}
}


/**
 * When called with avail and total set to nullptr this
 * function simply checks if we're warmed up. When the arguments
 * are not nullptr it counts the total pieces required for
 * warmup and the number of available pieces */
static inline int
__warmed(struct avbox_torrent * const inst, int64_t * const avail, int64_t * const total)
{
	if (inst->warmed) {
		return 1;
	}

	if (!inst->have_metadata) {
		return 0;
	}

	int piece_index, first_piece;
	const int stream_n_pieces = offset_to_piece_index(inst, inst->filesize - 1) + 1;

	/* reset counters */
	if (avail != nullptr) *avail = 0;
	if (total != nullptr) *total = 0;

	/* check that we have all the pieces at the end */
	first_piece = MAX(0, stream_n_pieces -
		((READAHEAD_TAIL + inst->piece_size - 1) / inst->piece_size) - 1);
	for (piece_index = first_piece; piece_index < stream_n_pieces; piece_index++) {
		if (!have_piece(inst, piece_index)) {
			if (total == nullptr) {
				return 0;
			} else {
				(*total) += piece_size(inst, piece_index);
			}
		} else {
			if (avail != nullptr) (*avail) += piece_size(inst, piece_index);
			if (total != nullptr) (*total) += piece_size(inst, piece_index);
		}
	}

	/* now check that we have readahead_min * 2 bytes from our
	 * current position */
	if (get_torrent_pos(inst) - inst->pos < inst->readahead_min * 2) {
		if (avail != nullptr && total != nullptr) {
			const int last_piece = offset_to_piece_index(inst,
				inst->pos + (inst->readahead_min * 2) - 1);
			for (piece_index = offset_to_piece_index(inst, inst->pos);
				piece_index < last_piece; piece_index++) {
				if (have_piece(inst, piece_index)) {
					(*avail) += piece_size(inst, piece_index);
				}
				(*total) += piece_size(inst, piece_index);
			}
		}
		return 0;
	} else {
		if (*avail < *total) {
			return 0;
		}
	}

	DEBUG_PRINT(LOG_MODULE, "Warmup complete");

	return inst->warmed = 1;
}


static int
warmed(struct avbox_torrent * const inst)
{
	/* this should optimize all the counting out
	 * of __warmed() */
	return __warmed(inst, nullptr, nullptr);
}


static void
adjust_priorities(struct avbox_torrent * const inst)
{
	ASSERT(inst->have_metadata);
	ASSERT(inst->handle.is_valid());

	const double bytes_per_sec = (double)inst->bitrate / 8;
	const int piece_duration = (int)(1000.0 / (bytes_per_sec / (double)inst->piece_size));
	const int stream_n_pieces = offset_to_piece_index(inst, inst->filesize - 1) + 1;
	int deadline = piece_duration, piece_index, first_piece;

	DEBUG_VPRINT(LOG_MODULE, "Adjusting piece priorities (piece_duration=%i)",
		piece_duration);

	/* prioritize pieces at the end */
	first_piece = MAX(0, stream_n_pieces -
		((READAHEAD_TAIL + inst->piece_size - 1) / inst->piece_size) - 1);
	DEBUG_VPRINT(LOG_MODULE, "Prioritizing pieces %d to %d",
		first_piece, stream_n_pieces - 1);
	for (piece_index = first_piece;
		piece_index >= 0 && piece_index < stream_n_pieces; piece_index++) {
		if (!have_piece(inst, piece_index)) {
			inst->handle.set_piece_deadline(piece_index, deadline, 0);
			deadline += piece_duration;
		}
	}

	/* next prioritize pieces starting at the current stream position */
	if (inst->pos < inst->filesize) {
		const int last_piece = first_piece;
		DEBUG_VPRINT(LOG_MODULE, "Prioritizing pieces %d to %d",
			offset_to_piece_index(inst, inst->pos), last_piece - 1);
		for (piece_index = first_piece = offset_to_piece_index(inst, inst->pos);
			piece_index < last_piece; piece_index++) {
			if (!have_piece(inst, piece_index)) {
				inst->handle.set_piece_deadline(piece_index, deadline, 0);
				deadline += piece_duration;
			}
		}
	}

	/* finally prioritize any remaining pieces */
	if (first_piece > 0) {
		DEBUG_VPRINT(LOG_MODULE, "Prioritizing pieces 0 to %d",
			first_piece - 1);
		for (piece_index = 0; piece_index < first_piece; piece_index++) {
			if (!have_piece(inst, piece_index)) {
				inst->handle.set_piece_deadline(piece_index, deadline, 0);
				deadline += piece_duration;
			}
		}
	}
}


static struct avbox_torrent *
find_stream(const lt::torrent_handle& torrent)
{
	struct avbox_torrent *stream, *ret = nullptr;
	pthread_mutex_lock(&session_lock);
	LIST_FOREACH(struct avbox_torrent*, stream, &torrents) {
		if (stream->handle == torrent) {
			ret = stream;
			break;
		}
	}
	pthread_mutex_unlock(&session_lock);
	return ret;
}


static struct avbox_torrent *
find_stream_by_info_hash(const lt::sha1_hash& hash)
{
	struct avbox_torrent *stream, *ret = nullptr;
	pthread_mutex_lock(&session_lock);
	LIST_FOREACH(struct avbox_torrent*, stream, &torrents) {
		if (stream->info_hash == lt::to_hex(hash.to_string())) {
			ret = stream;
			break;
		}
	}
	pthread_mutex_unlock(&session_lock);
	return ret;
}


static void
check_and_signal_piece_ready(struct avbox_torrent * inst, const int index)
{
	struct piece_status& piece = get_piece_status(inst, index);
	if (!inst->have_metadata || !piece.check_passed ||
		piece.blocks_finished < blocks_in_piece(inst, index)) {
		return;
	}

	ASSERT(!piece.ready);
	piece.ready = 1;
	inst->n_avail_pieces++;
	ASSERT(inst->n_avail_pieces <= inst->n_pieces);

	/* DEBUG_VPRINT(LOG_MODULE "-progress", "Piece %i ready", index); */

	/* signal readahead thread if it's waiting for this piece */
	if (!inst->have_metadata || !inst->warmed || inst->next_piece == index) {
		pthread_cond_signal(&inst->readahead_cond);
	}
}


static void
metadata_received(struct avbox_torrent * const inst)
{
	ASSERT(!inst->have_metadata);
	ASSERT(inst->torrent_size == 0);
	ASSERT(inst->handle.is_valid());

	FILE *f;
	std::string torrent_file;
	boost::shared_ptr<lt::torrent_info const> ti = inst->handle.torrent_file();


	/* save the torrent file */
	lt::create_torrent ct(*ti);
	lt::entry te = ct.generate();
	std::vector<char> buffer;
	bencode(std::back_inserter(buffer), te);

	torrent_file = torrents_path +
		lt::to_hex(ti->info_hash().to_string()) + ".torrent";

	if ((f = fopen(torrent_file.c_str(), "wb+")) == NULL) {
		/* LOG_VPRINT_ERROR("Could not save .torrent file: %s",
			(lt::to_hex(ti->info_hash().to_string()) + ".torrent").c_str()); */
	} else {
		fwrite(&buffer[0], 1, buffer.size(), f);
		fclose(f);
		DEBUG_VPRINT(LOG_MODULE, "Saved torrent to %s",
			torrent_file.c_str());
	}

	/* find the biggest file */
	int index = -1;
	lt::file_storage fs = ti->files();
	for (int i = 0; i < fs.num_files(); i++) {
		DEBUG_VPRINT(LOG_MODULE, "File %i: %s %" PRIi64 " %" PRIi64,
			i, fs.file_name(i).c_str(), fs.file_offset(i), fs.file_size(i));
		inst->torrent_size += fs.file_size(i);
		if (index == -1 || fs.file_size(i) > fs.file_size(index)) {
			index = i;
		}
	}

	/* update file info */
	inst->files_path = storage_path + "/" + ti->name();
	inst->filename = fs.file_name(index);
	inst->n_pieces = ti->num_pieces();
	inst->piece_size = ti->piece_length();
	inst->last_piece_size = ti->piece_size(inst->n_pieces - 1);
	inst->file_offset = fs.file_offset(index);
	inst->filesize = fs.file_size(index);
	inst->readahead_min = READAHEAD_MIN;
	inst->block_size = inst->handle.status().block_size;
	inst->blocks_per_piece = (inst->piece_size + inst->block_size - 1) / inst->block_size;
	inst->name = ti->name();

	/* this doesn't hold for all torrents. I think because
	 * of padding? */
	/* ASSERT(inst->torrent_size == (inst->n_pieces * inst->piece_size) -
		(inst->piece_size - inst->last_piece_size)); */

	pthread_mutex_lock(&inst->lock);

	const unsigned int n_rec_pieces = inst->avail_pieces.size();

	/* grow the list if necessary */
	if (n_rec_pieces < (unsigned int) inst->n_pieces) {
		inst->avail_pieces.resize(inst->n_pieces, { 0, 0 });
	}
	inst->have_metadata = 1;

	/* we can "receive" blocks before we've completed
	 * processing the metadata, but we cannot check if the
	 * piece is ready because blocks_in_piece() is only
	 * available after the metadata has been received. So
	 * now we need to check all the pieces for which we may
	 * have received blocks. */
	for (unsigned i = 0; i < n_rec_pieces; i++) {
		check_and_signal_piece_ready(inst, i);
	}

	adjust_priorities(inst);

	pthread_cond_signal(&inst->readahead_cond);
	pthread_mutex_unlock(&inst->lock);


	/* send notification of metadata received */
	if (inst->notify_object != nullptr) {
		if (avbox_object_sendmsg(&inst->notify_object,
			AVBOX_MESSAGETYPE_STREAM_READY, AVBOX_DISPATCH_UNICAST, inst) == nullptr) {
			LOG_VPRINT_ERROR("Could not send STREAM_READY message: %s",
				strerror(errno));
		}
	}

	DEBUG_VPRINT(LOG_MODULE, "Metadata received: (name=%s, piece_size=%i, n_pieces=%i, file=%s, path=%s)",
		inst->name.c_str(), inst->piece_size, inst->n_pieces,
		fs.file_path(index).c_str(),
		inst->files_path.c_str()); 
}


void
alerts_observer_plugin::on_alert(lt::alert const * a)
{
	/* a block has been written to disk */
	if (auto alert = lt::alert_cast<lt::block_finished_alert>(a)) {
		struct avbox_torrent *inst = find_stream(alert->handle);
		if (inst != nullptr) {
			pthread_mutex_lock(&inst->lock);
			struct piece_status& piece = get_piece_status(inst, alert->piece_index);
			piece.blocks_finished++;
			ASSERT(!inst->have_metadata || piece.blocks_finished <= blocks_in_piece(inst, alert->piece_index));
			check_and_signal_piece_ready(inst, alert->piece_index);
			pthread_mutex_unlock(&inst->lock);
		} else {
			DEBUG_PRINT(LOG_MODULE, "Could not find stream (block_finished_alert)!");
		}
	}

	/* a piece has failed the hash check */
	else if (auto alert = lt::alert_cast<lt::hash_failed_alert>(a)) {
		struct avbox_torrent *inst = find_stream(alert->handle);
		if (inst != nullptr) {
			pthread_mutex_lock(&inst->lock);
			struct piece_status& piece = get_piece_status(inst, alert->piece_index);
			ASSERT(!piece.check_passed && piece.blocks_finished == blocks_in_piece(inst, alert->piece_index));
			piece.blocks_finished = 0;
			DEBUG_VPRINT(LOG_MODULE, "Piece %i failed the hash check!", alert->piece_index);
			pthread_mutex_unlock(&inst->lock);
		} else {
			DEBUG_PRINT(LOG_MODULE, "Could not find stream (hash_failed_alert)!");
		}
	}

	/* a piece has passed the hash check */
	else if (auto alert = lt::alert_cast<lt::piece_finished_alert>(a)) {
		struct avbox_torrent *inst = find_stream(alert->handle);
		if (inst != nullptr) {
			pthread_mutex_lock(&inst->lock);
			struct piece_status& piece = get_piece_status(inst, alert->piece_index);
			piece.check_passed = 1;
			check_and_signal_piece_ready(inst, alert->piece_index);
			pthread_mutex_unlock(&inst->lock);
		} else {
			DEBUG_PRINT(LOG_MODULE, "Could not find stream (piece_finished_alert)!");
		}
	}

	/* torrent added */
	else if (auto alert = lt::alert_cast<lt::add_torrent_alert>(a)) {
		lt::torrent_handle h = alert->handle;
		if (h.is_valid()) {
			DEBUG_PRINT(LOG_MODULE, "Torrent added");
		}
	}

	/* received torrent metadata (ie magnet link resolved) */
	else if (auto alert = lt::alert_cast<lt::metadata_received_alert>(a)) {
		struct avbox_torrent *inst = find_stream(alert->handle);
		if (inst != nullptr) {
			if (avbox_object_sendmsg(&inst->object,
				AVBOX_TORRENTMSG_METADATA_RECEIVED, AVBOX_DISPATCH_UNICAST, inst) == nullptr) {
				LOG_VPRINT_ERROR("Could not send METADATA_RECEIVED message: %s",
					strerror(errno));
			}
		} else {
			DEBUG_PRINT(LOG_MODULE, "Could not find stream (metadata_received_alert)!");
		}
	}

	/* file error */
	else if (auto alert = lt::alert_cast<lt::file_error_alert>(a)) {
		LOG_VPRINT_ERROR("File error (%s): %s",
			alert->filename(), alert->error.message().c_str());
	}

	/* torrent error */
	else if (auto alert = lt::alert_cast<lt::torrent_error_alert>(a)) {
		LOG_VPRINT_ERROR("Torrent error (%s): %s",
			alert->torrent_name(), alert->what());
	}

	/* tracker error */
	else if (auto alert = lt::alert_cast<lt::tracker_error_alert>(a)) {
		LOG_VPRINT_ERROR("Tracker error (%s): %s",
			alert->tracker_url(), alert->error.message().c_str());
	}

	/* torrent finished */
	else if (auto alert = lt::alert_cast<lt::torrent_finished_alert>(a)) {
		struct avbox_torrent * const inst = find_stream(alert->handle);
		DEBUG_VPRINT(LOG_MODULE, "Torrent finished: %s",
			alert->message().c_str());
		if (inst != nullptr) {
			torrent_finished(inst);
		} else {
			DEBUG_PRINT(LOG_MODULE, "Could not find stream (torrent_finished_alert)!");
		}
	}

	/* torrent files moved */
	else if (auto alert = lt::alert_cast<lt::storage_moved_alert>(a)) {
		struct avbox_torrent * const inst = find_stream(alert->handle);
		if (inst != nullptr) {
			DEBUG_VPRINT(LOG_MODULE, "Storage moved: %s",
				inst->info_hash.c_str());
			if (inst->flags & AVBOX_TORRENTFLAGS_AUTOCLOSE) {
				DEBUG_PRINT(LOG_MODULE, "Moving storage automatically");
				avbox_torrent_close(inst);
			}
		} else {
			DEBUG_PRINT(LOG_MODULE, "Could not find stream (storage_moved_alert)!");
		}
	}

	/* could not move files */
	else if (auto alert = lt::alert_cast<lt::storage_moved_failed_alert>(a)) {
		struct avbox_torrent * const inst = find_stream(alert->handle);
		if (inst != nullptr) {
			LOG_VPRINT_ERROR("Could not move torrent files (%s): %s",
				inst->info_hash.c_str(), alert->error.message().c_str());
			if (inst->flags & AVBOX_TORRENTFLAGS_AUTOCLOSE) {
				DEBUG_PRINT(LOG_MODULE, "Moving storage automatically");
				avbox_torrent_close(inst);
			}
		} else {
			DEBUG_PRINT(LOG_MODULE, "Could not find stream (storage_moved_failed_alert)!");
		}
	}

	/* torrent removed */
	else if (auto alert = lt::alert_cast<lt::torrent_removed_alert>(a)) {
		struct avbox_torrent *inst =
			find_stream_by_info_hash(alert->info_hash);

		DEBUG_VPRINT(LOG_MODULE, "Torrent removed: %s",
			lt::to_hex(alert->info_hash.to_string()).c_str());

		if (inst != nullptr) {
			avbox_object_destroy(inst->object);
		} else {
			DEBUG_VPRINT(LOG_MODULE, "Could not get instance by info_hash: %s",
				lt::to_hex(alert->info_hash.to_string()).c_str());
		}
	}
}


static void*
readahead(void *arg)
{
	int fd = -1;
	struct avbox_torrent * const inst = (struct avbox_torrent*) arg;

	DEBUG_SET_THREAD_NAME("readahead");
	DEBUG_PRINT(LOG_MODULE, "Starting readahead worker");

	while (1) {

		pthread_mutex_lock(&inst->lock);

		/* exit if requested */
		if (inst->closed) {
			pthread_mutex_unlock(&inst->lock);
			break;
		}

		/* if there are already readahead_min bytes then wait
		 * until the user reads some */
		if (inst->have_metadata && (inst->ra_pos - inst->pos) > inst->readahead_min) {
			if (inst->user_waiting) {
				/* if the user thread is waiting then we must be either
				 * in underrun or not warmed up yet. So wake it up only
				 * after we've warmed up. Note that this code may run
				 * more than once after warmup if we wake up too early
				 * from pthread_cond_wait() or if the user thread doesn't
				 * wake up in time after we signal. So the assertion that
				 * '!inst->warmed || inst->underrun' may not always hold. */
				if (warmed(inst)) {
					pthread_cond_signal(&inst->user_cond);
				}
			}
			pthread_cond_wait(&inst->readahead_cond, &inst->lock);
			pthread_mutex_unlock(&inst->lock);
			continue;
		}

		if (inst->have_metadata && inst->ra_pos < inst->filesize &&
			have_piece(inst, (inst->next_piece = offset_to_piece_index(inst, inst->ra_pos)))) {

			const int64_t old_ra_pos = inst->ra_pos;
			const int real_sz = piece_size(inst, inst->next_piece);
			int sz = real_sz, buffer_offset = 0;
			ssize_t bytes_read;

			ASSERT(inst->ra_pos + inst->file_offset >= ((int64_t) inst->next_piece) * inst->piece_size);

			/* we're not reading the whole piece because we're only interested
			 * in the bytes starting at the current ra_pos and they may even be
			 * on another file in the case that (ra_pos == 0). This elimitanes
			 * the possibility of still using the piece if we seek within the
			 * same piece while reading, but that's very unlikely so for the sake
			 * of simplicity only read what we need */
			if (inst->ra_pos + inst->file_offset > (((int64_t) inst->next_piece) * inst->piece_size)) {
				const int64_t diff = inst->ra_pos + inst->file_offset - (((int64_t) inst->next_piece) * inst->piece_size);
				buffer_offset += diff;
				sz -= diff;
			}

			/* don't perform IO while owning the mutex */
			pthread_mutex_unlock(&inst->lock);

			/* open the file if necessary */
			if (fd == -1) {
				struct stat st;
				std::string filename = inst->files_path + "/" + inst->filename;

				/* if the file does not exist yet keep waiting */
				if (stat(filename.c_str(), &st) == -1) {
					LOG_VPRINT_ERROR("Could not stat file '%s': %s",
						filename.c_str(), strerror(errno));
					usleep(10LL * 1000LL); /* throttle RT */
					continue;
				}

				/* open the file */
				DEBUG_VPRINT(LOG_MODULE, "Opening file: %s",
					filename.c_str());
				if ((fd = open(filename.c_str(), O_CLOEXEC)) == -1) {
					LOG_VPRINT_ERROR("Could not open file '%s': %s",
						filename.c_str(), strerror(errno));
					usleep(10LL * 1000LL); /* throttle RT */
					continue;
				}
			}

			/* allocate a buffer for the piece */
			boost::shared_array<char> buffer(new char[inst->piece_size]);
			ASSERT(buffer != nullptr);

			/* read the piece */
			if ((bytes_read = pread(fd, &buffer[buffer_offset], sz, inst->ra_pos)) < sz) {
				if (bytes_read == -1) {
					LOG_VPRINT_INFO("Could not read piece from file (piece_index=%i offset=%" PRIi64 "): %s",
						inst->next_piece, inst->ra_pos, strerror(errno));
					usleep(10LL * 1000LL);	/* throttle RT */
					continue;
				} else if (inst->next_piece == offset_to_piece_index(inst, inst->filesize - 1) &&
					bytes_read >= (inst->filesize - inst->ra_pos)) {

					/* This is our last piece and we have read AT LEAST until what
					 * we think the end-of-file should be. However we may have read
					 * beyond that because the actual file may be bigger (an optimization
					 * done by libtorrent). So adjust the result in case of over-read
					 * to ensure that we don't set ra_pos beyond the end-of-file when
					 * incrementing bellow. */
					bytes_read = inst->filesize - inst->ra_pos;

				} else {
					DEBUG_VPRINT(LOG_MODULE, "pread() returned %d while expecting %d."
						"(ra_pos=%" PRIi64 " filesize=%" PRIi64 " to_eof=%" PRIi64 ") Will keep trying.",
						(int) bytes_read, (int) sz, inst->ra_pos, inst->filesize, inst->filesize - inst->ra_pos);
					usleep(10LL * 1000LL);	/* throttle RT */
					continue;
				}
			}

			/* TODO: Is there an external API to suggest a piece? */

			pthread_mutex_lock(&inst->lock);

			/* if a seek() happened while we were reading then
			 * we can no longer use this piece */
			if (inst->ra_pos != old_ra_pos) {
				DEBUG_VPRINT(LOG_MODULE, "Dropping read piece %i after seek",
					inst->next_piece);
				ASSERT(inst->readahead_pieces.size() == 0);
				pthread_mutex_unlock(&inst->lock);
				continue;
			}

			/* save the piece in the queue */
			boost::shared_ptr<struct piece_header> const piece(new struct piece_header());
			ASSERT(piece != nullptr);
			piece->buffer = buffer;
			piece->size = real_sz;
			piece->index = inst->next_piece;
			inst->readahead_pieces.push(piece);
			inst->ra_pos += bytes_read;

			ASSERT(inst->ra_pos <= inst->filesize);

			/* if the user thread is waiting for a piece then
			 * wake it up */
			if (inst->user_waiting && inst->warmed) {
				pthread_cond_signal(&inst->user_cond);
			}

			pthread_mutex_unlock(&inst->lock);
			continue;

		} else {
			/* NOTE: If you find yourself here (in the debugger) but you don't
			 * think you should be it's probably that optimizations are enabled
			 * and you're actually above waiting of the user thread */
			ASSERT(!inst->have_metadata || inst->ra_pos >= inst->filesize || !have_piece(inst, inst->next_piece));
			pthread_cond_wait(&inst->readahead_cond, &inst->lock);
			pthread_mutex_unlock(&inst->lock);
			continue;
		}
	}

	if (fd != -1) {
		close(fd);
	}

	DEBUG_VPRINT(LOG_MODULE, "Readahead thread for %s exiting",
		inst->info_hash.c_str());

	return NULL;
}


static int
control(void * const context, struct avbox_message * const msg)
{
	struct avbox_torrent * const inst =
		(struct avbox_torrent*) context;
	switch (avbox_message_id(msg)) {
	case AVBOX_TORRENTMSG_METADATA_RECEIVED:
	{
		if (find_stream(inst->handle) != nullptr) {
			metadata_received(inst);
		}
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		DEBUG_PRINT(LOG_MODULE, "Deleting torrent");
		pthread_mutex_lock(&session_lock);
		LIST_REMOVE(inst);
		pthread_mutex_unlock(&session_lock);
		return AVBOX_DISPATCH_OK;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		delete inst;
		return AVBOX_DISPATCH_OK;
	}
	default:
		ABORT("Invalid message received");
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Close a torrent stream.
 */
EXPORT void
avbox_torrent_close(struct avbox_torrent * const inst)
{
	DEBUG_PRINT(LOG_MODULE, "Closing torrent...");

	ASSERT(session != nullptr);

	pthread_mutex_lock(&inst->lock);
	inst->closed = 1;

	/* Wait for any threads blocked on avbox_torrent_read() to return */
	while (inst->user_waiting) {
		pthread_cond_signal(&inst->user_cond);
		pthread_mutex_unlock(&inst->lock);
		usleep(10LL * 1000LL);
		pthread_mutex_lock(&inst->lock);
	}

	pthread_mutex_unlock(&inst->lock);

	/* if this is a stream then wait for the readahead worker */
	if (inst->flags & AVBOX_TORRENTFLAGS_STREAM) {
		DEBUG_PRINT(LOG_MODULE, "Waiting for readahead thread to quit...");
		ASSERT(inst->readahead_fn != nullptr);
		pthread_cond_signal(&inst->readahead_cond);
		avbox_delegate_wait(inst->readahead_fn, nullptr);
		inst->readahead_fn = nullptr;
		avbox_thread_destroy(inst->readahead_thread);

		/* delete all cached pieces */
		while (!inst->readahead_pieces.empty()) {
			inst->readahead_pieces.pop();
		}
	}

	/* remove the torrent */
	if (inst->move_to.empty()) {
		session->remove_torrent(inst->handle, lt::session::delete_files);
	} else {
		session->remove_torrent(inst->handle);
	}

	DEBUG_PRINT(LOG_MODULE, "Stream closed");
}


/**
 * Read from a torrent stream.
 */
EXPORT int
avbox_torrent_read(struct avbox_torrent * const inst,
	uint8_t *buf, int sz)
{
	ASSERT(inst != NULL);
	ASSERT(buf != NULL);
	ASSERT(inst->flags & AVBOX_TORRENTFLAGS_STREAM);
	ASSERT(inst->readahead_thread != nullptr);

	pthread_mutex_lock(&inst->lock);

	/* if the stream is closed return error */
	if (inst->closed) {
		pthread_mutex_unlock(&inst->lock);
		errno = ESHUTDOWN;
		return -1;
	}

	/* if we're trying to read beyond the EOF return
	 * 0 */
	if (inst->have_metadata && inst->pos >= inst->filesize) {
		DEBUG_VPRINT(LOG_MODULE, "EOF reached (pos=%" PRIi64 " filesize=%" PRIi64 ")",
			inst->pos, inst->filesize);
		pthread_mutex_unlock(&inst->lock);
		return 0;
	}

	int64_t ra_min = MIN(inst->filesize - inst->pos, inst->readahead_min);
	int64_t ra_avail = inst->ra_pos - inst->pos;

	/* if we don't have enough bytes in ra wait */
	if (!inst->warmed || (inst->underrun && (ra_avail < ra_min)) || (ra_avail == 0)) {
		if (inst->warmed && (ra_avail == 0)) {
			inst->underrun = 1;
		}

		inst->user_waiting++;
		pthread_cond_wait(&inst->user_cond, &inst->lock);
		inst->user_waiting--;
		ra_min = MIN(inst->filesize - inst->pos, inst->readahead_min);
		ra_avail = inst->ra_pos - inst->pos;

		if (!inst->warmed || (inst->underrun && (ra_avail < ra_min)) || (ra_avail == 0)) {
			pthread_mutex_unlock(&inst->lock);
			errno = EAGAIN;
			return -1;
		}

		inst->underrun = 0;
	}

	/* if we're trying to read beyond the EOF return 0 */
	if (inst->pos >= inst->filesize) {
		DEBUG_VPRINT(LOG_MODULE, "EOF reached (pos=%" PRIi64 " filesize=%" PRIi64 ")",
			inst->pos, inst->filesize);
		pthread_mutex_unlock(&inst->lock);
		return 0;
	}

	const int piece_index = offset_to_piece_index(inst, inst->pos);
	ASSERT(!inst->readahead_pieces.empty());

	/* if we're done with the front piece then pop it */
	if (inst->readahead_pieces.front()->index != piece_index) {
		inst->readahead_pieces.pop();
		ASSERT(!inst->readahead_pieces.empty());
		ASSERT(inst->readahead_pieces.front()->index == piece_index);
	}

	/* copy the bytes requested */
	const int offset = (inst->pos + inst->file_offset) - (((int64_t) piece_index) * inst->piece_size);
	const int bytes_to_read = (piece_index == offset_to_piece_index(inst, inst->filesize - 1)) ?
		MIN(sz, inst->filesize - inst->pos) : MIN(sz, inst->readahead_pieces.front()->size - offset);
	ASSERT(bytes_to_read <= sz);
	memcpy(buf, &inst->readahead_pieces.front()->buffer[offset], bytes_to_read);

	/* signal the readahead thread if it may be waiting
	 * for us */
	if (1 || (inst->ra_pos - inst->pos) >= inst->readahead_min) {
		pthread_cond_signal(&inst->readahead_cond);
	}

	/* increment pos and make sure we don't over-read */
	inst->pos += bytes_to_read;
	ASSERT(inst->pos <= inst->filesize);

	/* DEBUG_VPRINT(LOG_MODULE, "Read %i bytes from piece %i at offset %i (pos=%" PRIi64 " offset=%" PRIi64 " piece_size=%" PRIi64
		"ra_pos=%" PRIi64 " count=%" PRIi64 ")",
		bytes_to_read, piece_index, offset, inst->pos - bytes_to_read, inst->file_offset, inst->piece_size,
		inst->ra_pos, inst->ra_pos - inst->pos); */

	pthread_mutex_unlock(&inst->lock);

	return bytes_to_read;
}


EXPORT void
avbox_torrent_bufferstate(struct avbox_torrent * const inst,
	int64_t * const count, int64_t * const capacity)
{
	if (__warmed(inst, count, capacity)) {

		ASSERT(inst->have_metadata);

		const int64_t pos = inst->pos;
		const int64_t torrent_pos = get_torrent_pos(inst);

		*count = *capacity = inst->readahead_min;

		if (torrent_pos < inst->filesize &&
			(pos + inst->readahead_min) < inst->filesize) {
			const int piece_index = offset_to_piece_index(inst, torrent_pos);
			const int last_piece = offset_to_piece_index(inst,
				MIN(inst->filesize, pos + inst->readahead_min));
			*count = MIN(inst->readahead_min, torrent_pos - pos);
			for (int i = piece_index; i < last_piece; i++) {
				if (have_piece(inst, i)) {
					*count += inst->piece_size;
				}
			}
			if (*count > *capacity) {
				*count = *capacity;
			}
		}
	}
}


EXPORT int64_t
avbox_torrent_downloaded(const struct avbox_torrent * const inst)
{
	if (!inst->have_metadata) {
		return 0;
	}

	return (((int64_t) inst->n_avail_pieces) * inst->piece_size) -
		(have_piece(inst, inst->n_pieces - 1) ? (inst->piece_size - inst->last_piece_size) : 0);
}


EXPORT int64_t
avbox_torrent_tell(const struct avbox_torrent * const inst)
{
	return inst->pos;
}


EXPORT int64_t
avbox_torrent_size(const struct avbox_torrent * const inst)
{
	if (!inst->have_metadata) {
		DEBUG_PRINT(LOG_MODULE, "Size requested before metadata! Returning -1");
		return -1;
	} else {
		return inst->filesize;
	}
}


EXPORT const char *
avbox_torrent_name(const struct avbox_torrent * const inst)
{
	if (inst->name == "") {
		return inst->info_hash.c_str();
	} else {
		return inst->name.c_str();
	}
}


EXPORT const char *
avbox_torrent_id(const struct avbox_torrent * const inst)
{
	return inst->info_hash.c_str();
}


EXPORT int
avbox_torrent_seek(struct avbox_torrent * const inst,
	const int64_t pos)
{

	DEBUG_VPRINT(LOG_MODULE, "Seeking to position %" PRIi64,
		pos);

	ASSERT(inst != NULL);
	ASSERT(inst->flags & AVBOX_TORRENTFLAGS_STREAM);

	if (pos > inst->filesize) {
		DEBUG_PRINT(LOG_MODULE, "Return -1 to seek beyond EOF");
		return -1;
	}

	pthread_mutex_lock(&inst->lock);

	if (inst->closed) {
		pthread_mutex_unlock(&inst->lock);
		return -1;
	}

	/* delete all cached pieces */
	while (!inst->readahead_pieces.empty()) {
		inst->readahead_pieces.pop();
	}

	/* update the position and priorities */
	inst->pos = inst->ra_pos = pos;
	if (inst->have_metadata) {
		adjust_priorities(inst);
	}

	/* wake the readahead thread */
	pthread_cond_signal(&inst->readahead_cond);

	DEBUG_VPRINT(LOG_MODULE, "Returning from seek: %" PRIi64,
		inst->pos);

	pthread_mutex_unlock(&inst->lock);

	return 0;
}


EXPORT void
avbox_torrent_setbitrate(struct avbox_torrent * const inst, const int bitrate)
{
	inst->bitrate = bitrate;
	if (inst->have_metadata) {
		pthread_mutex_lock(&inst->lock);
		adjust_priorities(inst);
		pthread_mutex_unlock(&inst->lock);
	}
}


EXPORT struct avbox_torrent*
avbox_torrent_next(struct avbox_torrent * const current)
{
	/* FIXME: We need to check that the torrent object is still in the list
	 * by iterating through each item in the list */
	struct avbox_torrent *next = nullptr;

	pthread_mutex_lock(&session_lock);

	if (current == nullptr) {
		next = LIST_NEXT(struct avbox_torrent*, (struct avbox_torrent*) &torrents);
	} else {
		bool found = false;;
		struct avbox_torrent *stream;
		LIST_FOREACH(struct avbox_torrent*, stream, &torrents) {
			if (current == stream) {
				found = true;
				break;
			}
		}
		if (!found) {
			/* the stream was removed from the list */
			goto end;
		}
		next = LIST_NEXT(struct avbox_torrent*, current);
	}
	if (LIST_ISNULL(&torrents, next)) {
		next = nullptr;
		goto end;
	} else {
		if (avbox_object_ref(next->object) != next->object) {
			DEBUG_ABORT(LOG_MODULE, "WTF?");
		}
	}
end:
	pthread_mutex_unlock(&session_lock);
	return next;
}


EXPORT void
avbox_torrent_unref(struct avbox_torrent * const inst)
{
	avbox_object_unref(inst->object);
}


EXPORT void
avbox_torrent_moveonfinish(struct avbox_torrent * const inst,
	const char * const dest)
{
	ASSERT(inst != NULL);
	ASSERT(dest != NULL);
	inst->move_to = std::string(dest);
	inst->flags |= AVBOX_TORRENTFLAGS_AUTOCLOSE;
	DEBUG_VPRINT(LOG_MODULE, "Torrent %s will be moved to %s when finished",
		inst->info_hash.c_str(), inst->move_to.c_str());
}


/**
 * Open a torrent stream stream.
 */
EXPORT struct avbox_torrent*
avbox_torrent_open(const char * const uri, const char * const file,
	unsigned int flags, struct avbox_object * const notify_object)
{
	FILE *f = NULL;
	struct avbox_torrent *inst;
	pthread_mutexattr_t lockattr;
	libtorrent::add_torrent_params params;
	std::string torrent_filename = "";
	std::string suri(uri);
	lt::error_code ec;

	ASSERT(session != nullptr);

	/* if this is a url to a torrent file fetch it and save
	 * it to temp file */
	if (!strncmp("http", uri, 4)) {
		LOG_PRINT_ERROR("This is an http url to a torrent file!");
		uint8_t *buf = NULL;
		size_t sz = 1024 * 1024 * 1;	/* cap torrent size at 1MiB */

		if (avbox_net_geturl(uri, (void**) &buf, &sz) == -1) {
			LOG_VPRINT_ERROR("Could not fetch torrent file (%s): %s",
				uri, strerror(errno));
			return NULL;
		}
		if (sz <= 0) {
			LOG_VPRINT_ERROR("Fetch succeeded but sz == %z",
				sz);
			if (buf != NULL) {
				free(buf);
			}
			return NULL;
		}

		ASSERT(buf != NULL);

		torrent_filename = std::string(tmpnam(NULL));

		if ((f = fopen(torrent_filename.c_str(), "wb")) == NULL) {
			LOG_VPRINT_ERROR("Could not save torrent to temp file: %s",
				strerror(errno));
			free(buf);
			return NULL;
		}

		if (fwrite(buf, 1, sz, f) < sz) {
			LOG_VPRINT_ERROR("Could not write to torrent file: %s",
				strerror(errno));
			free(buf);
			unlink(torrent_filename.c_str());
			return NULL;
		}

		fflush(f);
		fclose(f);
		f = NULL;
		free(buf);

		DEBUG_VPRINT(LOG_MODULE, "Torrent file fetched to %s",
			torrent_filename.c_str());
	}

	/* allocate stream context */
	if ((inst = new struct avbox_torrent()) == nullptr) {
		if (torrent_filename != "") {
			unlink(torrent_filename.c_str());
		}
		return NULL;
	}

	/* initialize mutexes */
	pthread_mutexattr_init(&lockattr);
	pthread_mutexattr_setprotocol(&lockattr, PTHREAD_PRIO_INHERIT);
	if (pthread_mutex_init(&inst->lock, &lockattr) != 0) {
		ABORT("Could not initialize pthread primitives!");
	}
	pthread_mutexattr_destroy(&lockattr);

	/* initialize condition variables */
	if (pthread_cond_init(&inst->readahead_cond, nullptr) != 0 ||
		pthread_cond_init(&inst->user_cond, nullptr) != 0) {
		ABORT("Could not initialize pthread primitives!");
	}

	/* initialize stream */
	inst->pos = 0;
	inst->underrun = 0;
	inst->n_pieces = 0;
	inst->n_avail_pieces = 0;
	inst->torrent_size = 0;
	inst->flags = flags;
	inst->notify_object = notify_object;
	inst->ra_pos = 0;
	inst->readahead_fn = nullptr;
	inst->bitrate = 12000000; /* about 12 Mbps for h264 1080p at 60Hz */

	/* add the torrent to the session */
	if (!torrent_filename.empty()) {
		params.ti = boost::make_shared<lt::torrent_info>(
			torrent_filename, boost::ref(ec), 0);
		if (ec) {
			LOG_VPRINT_ERROR("Could not create torrent_info: %s",
				ec.message().c_str());
			free(inst);
			unlink(torrent_filename.c_str());
			return NULL;
		}
	} else {
		params.url = suri;
	}

	/* create object */
	if ((inst->object = avbox_object_new(&control, inst)) == NULL) {
		LOG_VPRINT_ERROR("Could not create object: %s",
			strerror(errno));
		free(inst);
		if (!torrent_filename.empty()) {
			unlink(torrent_filename.c_str());
		}
		return NULL;
	}

	params.save_path = storage_path;
	params.storage_mode = lt::storage_mode_allocate;

	/* add the torrent */
	pthread_mutex_lock(&inst->lock);
	pthread_mutex_lock(&session_lock);
	LIST_ADD(&torrents, inst);
	inst->handle = session->add_torrent(params, boost::ref(ec));
	if (ec) {
		LOG_VPRINT_ERROR(LOG_MODULE, "Could not add torrent: %s",
			ec.message().c_str());
		LIST_REMOVE(inst);
		pthread_mutex_unlock(&inst->lock);
		pthread_mutex_destroy(&inst->lock);
		free(inst);
		if (!torrent_filename.empty()) {
			unlink(torrent_filename.c_str());
		}
		pthread_mutex_unlock(&session_lock);
		return NULL;
	}

	ASSERT(inst->handle.is_valid());

	/* save info hash */
	inst->info_hash = lt::to_hex(inst->handle.info_hash().to_string());

	pthread_mutex_unlock(&session_lock);
	pthread_mutex_unlock(&inst->lock);

	/* if this is a temporary torrent then unlink it
	 * and call metadata_received */
	if (!torrent_filename.empty()) {
		unlink(torrent_filename.c_str());
		metadata_received(inst);
	}

	DEBUG_VPRINT(LOG_MODULE, "Torrent added: info_hash=%s",
		inst->info_hash.c_str());

	if (flags & AVBOX_TORRENTFLAGS_STREAM) {
		DEBUG_PRINT(LOG_MODULE, "Starting readahead thread");

		/* start the readahead thread */
		if ((inst->readahead_thread = avbox_thread_new(NULL, NULL, AVBOX_THREAD_REALTIME, -10)) == nullptr) {
			ABORT("Could not initialize readahead thread!");
		}
		if ((inst->readahead_fn = avbox_thread_delegate(inst->readahead_thread, readahead, inst)) == nullptr) {
			ABORT("Could not start readahead worker");
		}
	}

	return inst;
}


int
avbox_torrent_init(void)
{
	lt::settings_pack settings;
	struct stat st;
	pthread_mutexattr_t lockattr;

	DEBUG_PRINT(LOG_MODULE, "Creating session " STRINGIZE(LOCALSTATEDIR));

	pthread_mutexattr_init(&lockattr);
	pthread_mutexattr_setprotocol(&lockattr, PTHREAD_PRIO_INHERIT);
	if (pthread_mutex_init(&session_lock, &lockattr) != 0) {
		ABORT("Could not initialize pthread primitives!");
	}
	pthread_mutexattr_destroy(&lockattr);

#ifdef ENABLE_REALTIME
	int have_old_policy = 1;
	int old_policy = SCHED_RR;
	struct sched_param parms;
	struct sched_param old_parms;

	/* save current thread priority */
	if (pthread_getschedparam(pthread_self(), &old_policy, &old_parms) != 0) {
		LOG_PRINT_ERROR("Could not get main thread priority");
		have_old_policy = 0;
	}
	parms.sched_priority = sched_get_priority_min(SCHED_RR);
	if (pthread_setschedparam(pthread_self(), SCHED_RR, &parms) != 0) {
		LOG_PRINT_ERROR("Could not set main thread priority");
	}
#endif

	LIST_INIT(&torrents);

	/* ensure that torrents and downloads directories exist */
	if (stat(storage_path.c_str(), &st) == -1) {
		if (mkdir_p(storage_path.c_str(), S_IRWXU) == -1) {
			LOG_VPRINT_ERROR("Could not create path: %s (%s)",
				storage_path.c_str(), strerror(errno));
		}
	}
	if (stat(torrents_path.c_str(), &st) == -1) {
		if (mkdir_p(torrents_path.c_str(), S_IRWXU) == -1) {
			LOG_VPRINT_ERROR("Could not create path: %s (%s)",
				torrents_path.c_str(), strerror(errno));
		}
	}

	settings.set_int(lt::settings_pack::alert_mask,
		lt::alert::progress_notification |
		lt::alert::error_notification |
		lt::alert::storage_notification |
		lt::alert::status_notification |
		lt::alert::peer_notification);

	/* we're using an alerts observer. No queue needed */
	settings.set_int(lt::settings_pack::alert_queue_size, 0);

	/* the page cause causes latency issues for RT and it's also
	 * pretty pointless on Linux */
	settings.set_int(lt::settings_pack::read_cache_line_size, 0);
	settings.set_int(lt::settings_pack::write_cache_line_size, 1);

	/* tune for steady piece rate */
	settings.set_int(lt::settings_pack::request_queue_time, 1); /* default is 3 */
	settings.set_int(lt::settings_pack::max_out_request_queue, 100); /* default is 500 */
	settings.set_int(lt::settings_pack::whole_pieces_threshold, 5); /* default is 20 */
	settings.set_int(lt::settings_pack::peer_timeout, 60); /* default is 120 */

#if 1 || defined(ENABLE_REALTIME)
	settings.set_bool(lt::settings_pack::low_prio_disk, true);
	settings.set_int(lt::settings_pack::aio_threads, 1);
	settings.set_int(lt::settings_pack::aio_max, 10);
	settings.set_int(lt::settings_pack::listen_queue_size, 1);
#endif

#if 0
	/* enable uTP and TCP and uTP only for outgoing */
	settings.set_bool(lt::settings_pack::enable_outgoing_utp, true);
	settings.set_bool(lt::settings_pack::enable_outgoing_tcp, true);
	settings.set_bool(lt::settings_pack::enable_incoming_utp, true);
	settings.set_bool(lt::settings_pack::enable_incoming_tcp, true);
#endif

	/* cleanup temp directory */
	cleanup_temp_directory();

	/* initialize libtorrent */
	if ((session = new lt::session(settings)) == nullptr) {
		DEBUG_PRINT(LOG_MODULE, "Could not create libtorrent session!");
		return -1;
	}

	/* set the alert handler */
	boost::shared_ptr<lt::plugin> aop =
		boost::make_shared<struct alerts_observer_plugin>();
	session->add_extension(aop);

	/* include everything in global limits */
	lt::peer_class_type_filter peer_classes;
	peer_classes.add(lt::peer_class_type_filter::tcp_socket, lt::session::global_peer_class_id);
	peer_classes.add(lt::peer_class_type_filter::ssl_tcp_socket, lt::session::global_peer_class_id);
	peer_classes.add(lt::peer_class_type_filter::i2p_socket, lt::session::global_peer_class_id);
	peer_classes.add(lt::peer_class_type_filter::utp_socket, lt::session::global_peer_class_id);
	peer_classes.add(lt::peer_class_type_filter::ssl_utp_socket, lt::session::global_peer_class_id);
	session->set_peer_class_type_filter(peer_classes);

#ifdef ENABLE_REALTIME
	/* restore old scheduling policy */
	if (have_old_policy) {
		if (pthread_setschedparam(pthread_self(), old_policy, &old_parms) != 0) {
			LOG_PRINT_ERROR("Could not set main thread priority");
		}
	}
#endif

	return 0;
}


void
avbox_torrent_shutdown(void)
{
	if (session != nullptr) {
		pthread_mutex_lock(&session_lock);
		if (LIST_SIZE(&torrents) != 0) {
			DEBUG_VPRINT(LOG_MODULE, "There are still %i items in the list!",
				LIST_SIZE(&torrents));
		}
		pthread_mutex_unlock(&session_lock);

		quit = 1;
		delete session;
		session = nullptr;
		quit = 0;
	}
}
