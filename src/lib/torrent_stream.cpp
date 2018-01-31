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


namespace lt = libtorrent;

struct cache_entry
{
	int size;
	int index;	/* only needed for debugging */
	boost::shared_array<char> buffer;
};


LISTABLE_STRUCT(avbox_torrent,
	int have_metadata;
	int n_pieces;
	int block_size;
	int closed;
	int blocking;
	int download_ahead_min;
	int underrun;
	int warm;
	int warm_pieces;
	int warmup_pieces;
	unsigned int flags;

	int64_t offset;		/* the offset of the file that we're streaming */
	int64_t filesize;	/* the size of the file we're streaming */
	int64_t pos;		/* the stream position */
	int64_t torrent_pos;	/* the position after the last sequential byte downloaded */
	int64_t ra_pos;		/* the current readahead position */

	int ra_seeking;
	int ra_halted;
	int ra_current_piece;
	int ra_current_piece_size;
	boost::shared_array<char> ra_current_piece_buffer;
	boost::shared_array<char> ra_temp_buffer;
	int ra_temp_buffer_size;
	int ra_temp_buffer_index;
	pthread_cond_t ra_piece_read_cond;
	pthread_cond_t ra_cond;
	std::queue<struct cache_entry*> ra_pieces;
	struct avbox_thread *ra_thread;	/* the readahead thread */
	struct avbox_delegate *ra_worker;	/* the readahead worker */

	struct avbox_object *object;
	struct avbox_object *notify_object;
	lt::torrent_handle handle;
	std::string name;
	std::string info_hash;
	std::string files_path;
	std::string move_to;
	pthread_mutex_t lock;
	pthread_cond_t cond;
);


static int quit = 0;
static lt::session *session = nullptr;
static struct avbox_thread *thread;
static LIST torrents;
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;

static const std::string storage_path(STRINGIZE(LOCALSTATEDIR) "/lib/mediabox/store/downloads");
static const std::string torrents_path(std::string(STRINGIZE(LOCALSTATEDIR)) + "/lib/mediabox/torrents/");


static int
offset_to_piece_index(struct avbox_torrent * const inst, int64_t offset)
{
	return (offset + inst->offset) / inst->block_size;
}


static void
torrent_finished(struct avbox_torrent * const inst)
{
	bool finished = true;
	for (int i = 0; i < inst->n_pieces; i++) {
		if (!inst->handle.have_piece(i)) {
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
			inst->handle.move_storage(inst->move_to,
				lt::always_replace_files);
			DEBUG_VPRINT(LOG_MODULE, "Storage move requested for %s",
				inst->info_hash.c_str());
		}
	}
}


/**
 * Tell libtorrent to prioritize chunks starting at pos.
 */
static void
adjust_priorities(struct avbox_torrent * const inst)
{
	/* only prioritize torrents with the AVBOX_TORRENTFLAGS_STREAM.
	 * All other torrents keep their default priority (1) so torrents
	 * with this flag are prioritized above all others. Therefore it should
	 * be OK to download torrents while playing a stream. */
	if (!(inst->flags & AVBOX_TORRENTFLAGS_STREAM)) {
		return;
	}

	/* set the priority of the next N_PIECES pieces to 7 */
	if (inst->warm) {
		int i;
		int next_piece = offset_to_piece_index(inst, inst->torrent_pos);
		const int N_PIECES = (inst->download_ahead_min / inst->block_size) + 1;
		const int next_piece_copy = next_piece;
		for (i = 0; i < N_PIECES; next_piece++) {
			if (next_piece >= inst->n_pieces) {
				break;
			}
			if (!inst->handle.have_piece(next_piece)) {
				inst->handle.piece_priority(next_piece, 7);
				inst->handle.set_piece_deadline(next_piece, 250 + (i * 10), 0);
				i++;
			}
		}

		/* If we're done downloading the file that we want set the
		 * priority of the others to normal */
		if (i == 0) {
			bool finished = true;
			DEBUG_VPRINT(LOG_MODULE, "End of file reached (next_piece=%i next_piece_copy=%i n_pieces=%i)",
				next_piece, next_piece_copy, inst->n_pieces);
			for (i = 0; i < next_piece_copy; i++) {
				if (!inst->handle.have_piece(i)) {
					inst->handle.piece_priority(i, 1);
					DEBUG_VPRINT(LOG_MODULE, "Setting priority of piece %i", i);
					finished = false;
				}
			}
			if (finished) {
				torrent_finished(inst);
			}
		}

		#if 0
		if (inst->flags & AVBOX_TORRENTFLAGS_STREAM) {
			if (inst->bitrate > 0) {
				inst->handle.set_upload_rate(inst->bitrate * 2);
			} else {

			}
		} else {
			inst->handle.set_download_limit(1024 * 5);
		}
		#endif
	} else {
		int tm = 0, warm_pieces = 0;
		int next_piece = offset_to_piece_index(inst, 0);
		const int N_PIECES = ((inst->download_ahead_min * 2) / inst->block_size) + 1;
		const int TAIL_PIECES = (1024 * 1024 * 5) / inst->block_size;

		/* prioritize pieces at the current position */
		for (int i = 0; i < N_PIECES; i++, next_piece++) {
			if (next_piece >= inst->n_pieces) {
				break;
			}
			if (!inst->handle.have_piece(next_piece)) {
				inst->handle.piece_priority(next_piece, 7);
				inst->handle.set_piece_deadline(next_piece, 250 + (tm * 10), 0);
				tm++;
			} else {
				warm_pieces++;
			}
		}

		/* prioritize pieces at the end */
		next_piece = offset_to_piece_index(inst, inst->filesize - 1);
		for (int i = 0; i < TAIL_PIECES; i++, next_piece--) {
			if (!inst->handle.have_piece(next_piece)) {
				inst->handle.piece_priority(next_piece, 7);
				inst->handle.set_piece_deadline(next_piece, 250 + (tm * 10), 0);
				tm++;
			} else {
				warm_pieces++;
			}
		}

		inst->warmup_pieces = N_PIECES + TAIL_PIECES;
		inst->warm_pieces = warm_pieces;

		if (tm == 0) {
			pthread_cond_broadcast(&inst->ra_cond);
			DEBUG_PRINT(LOG_MODULE, "Warmup complete");
			inst->warm = 1;
			adjust_priorities(inst);
		}
	}
}


static int
update_torrent_pos(struct avbox_torrent * const inst)
{
	int ret = 0;

	if (inst->handle.is_valid()) {

		int next_piece = offset_to_piece_index(inst, inst->torrent_pos);
		lt::torrent_handle &h = inst->handle;

		boost::shared_ptr<lt::torrent_info const> ti =
			inst->handle.torrent_file();

		if (next_piece >= inst->n_pieces) {
			return 0;
		}

		/* check how many successive pieces are available
		 * and update the position accordingly */
		while (h.have_piece(next_piece)) {
			if (next_piece == (inst->offset / inst->block_size)) {
				DEBUG_VPRINT(LOG_MODULE, "Handling first piece (%i)",
					next_piece);
				/* this is wrong and only works if the offset of the
				 * file that we want is within the first piece */
				inst->torrent_pos += ti->piece_size(next_piece) - inst->offset;
			} else {
				inst->torrent_pos += ti->piece_size(next_piece);
			}
			next_piece++;
			ret = 1;
			if (next_piece >= inst->n_pieces) {
				break;
			}
		}
	}

	return ret;
}


static struct avbox_torrent *
find_stream(const lt::torrent_handle& torrent)
{
	struct avbox_torrent *stream, *ret = nullptr;
	pthread_mutex_lock(&session_lock);
	LIST_FOREACH(struct avbox_torrent*, stream, &torrents) {
		if (stream->handle.is_valid() && stream->handle == torrent) {
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
metadata_received(struct avbox_torrent * const inst)
{
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
		if (index == -1 || fs.file_size(i) > fs.file_size(index)) {
			index = i;
		}
	}

	/* update file info */
	pthread_mutex_lock(&inst->lock);
	inst->files_path = storage_path + "/" + ti->name();
	inst->n_pieces = ti->num_pieces();
	inst->block_size = ti->piece_length();
	inst->offset = fs.file_offset(index);
	inst->filesize = fs.file_size(index);
	inst->download_ahead_min = 1024 * 1024 * 15;
	inst->warmup_pieces = 0;
	inst->name = ti->name();
	inst->have_metadata = 1;
	adjust_priorities(inst);
	pthread_cond_broadcast(&inst->ra_cond);

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
		inst->name.c_str(), inst->block_size, inst->n_pieces,
		fs.file_path(index).c_str(),
		inst->files_path.c_str()); 
}


static void *
session_monitor(void *data)
{
	DEBUG_SET_THREAD_NAME("torrent_session");
	DEBUG_PRINT(LOG_MODULE, "Starting torrent monitor");

	(void) data;

	while (!quit) {

		std::vector<lt::alert*> alerts;

		/* wait for an alert */
		if (session->wait_for_alert(std::chrono::seconds(1)) == nullptr) {
			continue;
		}

		session->pop_alerts(&alerts);

		for (lt::alert const * a : alerts) {

			/* torrent added */
			if (auto alert = lt::alert_cast<lt::add_torrent_alert>(a)) {
				lt::torrent_handle h = alert->handle;
				if (h.is_valid()) {
					DEBUG_PRINT(LOG_MODULE, "Torrent added");
				}
			}

			/* received torrent metadata (ie magnet link resolved) */
			else if (auto alert = lt::alert_cast<lt::metadata_received_alert>(a)) {
				struct avbox_torrent *inst = find_stream(alert->handle);
				if (inst != nullptr) {
					/* NOTE: metadata_received() takes the lock */
					metadata_received(inst);
				} else {
					DEBUG_PRINT(LOG_MODULE, "Could not find stream (metadata_received_alert)!");
				}
			}

			/* a piece has finished downloading */
			else if (auto alert = lt::alert_cast<lt::piece_finished_alert>(a)) {
				struct avbox_torrent * const inst = find_stream(alert->handle);
				if (inst != nullptr) {
					/* DEBUG_VPRINT(LOG_MODULE, "Piece received %i",
						alert->piece_index); */
					pthread_mutex_lock(&inst->lock);
					adjust_priorities(inst);
					update_torrent_pos(inst);
					pthread_cond_broadcast(&inst->ra_cond);
					pthread_mutex_unlock(&inst->lock);
				} else {
					DEBUG_PRINT(LOG_MODULE, "Could not find stream (piece_finished_alert)!");
				}
			}

			/* piece read complete */
			else if (auto alert = lt::alert_cast<lt::read_piece_alert>(a)) {
				struct avbox_torrent * const inst = find_stream(alert->handle);
				if (inst != nullptr) {
					inst->ra_temp_buffer = alert->buffer;
					inst->ra_temp_buffer_size = alert->size;
					inst->ra_temp_buffer_index = alert->piece;

					/* copy the piece to the current buffer and
					 * signal waiting thread */
					pthread_mutex_lock(&inst->lock);
					pthread_cond_signal(&inst->ra_piece_read_cond);
					pthread_mutex_unlock(&inst->lock);
				} else {
					DEBUG_PRINT(LOG_MODULE, "Could not find stream (read_piece_alert)!");
				}
			}

			/* file error */
			else if (auto alert = lt::alert_cast<lt::file_error_alert>(a)) {
				LOG_VPRINT_ERROR("File error (%s): %s",
					alert->filename(), alert->what());
			}

			/* torrent error */
			else if (auto alert = lt::alert_cast<lt::torrent_error_alert>(a)) {
				LOG_VPRINT_ERROR("Torrent error (%s): %s",
					alert->torrent_name(), alert->what());
			}

			/* torrent finished */
			else if (auto alert = lt::alert_cast<lt::torrent_finished_alert>(a)) {
				struct avbox_torrent * const inst = find_stream(alert->handle);

				DEBUG_VPRINT(LOG_MODULE, "Torrent finished: %s",
					alert->message().c_str());

				/* All the requested pieces are finished downloading. We may receive
				 * this several times befere we're finished with the torrent due to
				 * prioritization race conditions */
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
	}

	DEBUG_PRINT(LOG_MODULE, "Torrent monitor exitting");

	return nullptr;
}


static void*
readahead_thread(void *arg)
{
	struct avbox_torrent * const inst = (struct avbox_torrent*) arg;

	DEBUG_SET_THREAD_NAME("bt-readahead");
	DEBUG_PRINT(LOG_MODULE, "Starting readahead worker");

	while (!inst->closed) {

		const int next_piece = offset_to_piece_index(inst, inst->ra_pos);

		pthread_mutex_lock(&inst->lock);

		/* if there are already 10mb in readahead buffer wait
		 * until the user reads some */
		if ((inst->ra_pos - inst->pos) > inst->download_ahead_min ||
			(inst->have_metadata && inst->ra_pos >= inst->filesize)) {
			/* wait until user reads */
			pthread_cond_wait(&inst->ra_cond, &inst->lock);
			pthread_cond_signal(&inst->ra_cond);
			pthread_mutex_unlock(&inst->lock);
			continue;
		}

		if (inst->have_metadata && inst->handle.have_piece(next_piece)) {
			/* request the piece and wait for it to be ready */
			boost::shared_ptr<lt::torrent_info const> ti =
				inst->handle.torrent_file();
			const int sz = ti->piece_size(next_piece);
			const int64_t old_pos = inst->ra_pos;

			/* Request a piece read and wait for it to complete */
			inst->handle.read_piece(next_piece);
			pthread_cond_wait(&inst->ra_piece_read_cond, &inst->lock);

			/* make sure that we weren't interrupted by seek()
			 * while we where waiting */
			if (inst->ra_pos == old_pos) {
				struct cache_entry * const entry = new struct cache_entry();
				entry->buffer = inst->ra_temp_buffer;
				entry->size = inst->ra_temp_buffer_size;
				entry->index = next_piece;
				inst->ra_pieces.push(entry);

				/* DEBUG_VPRINT(LOG_MODULE, "Adding piece %i to queue (requested %i)",
					inst->ra_temp_buffer_index, next_piece); */

				if (inst->ra_temp_buffer_index != next_piece) {
					DEBUG_VPRINT(LOG_MODULE, "Expected piece %i but got %i",
						next_piece, inst->ra_temp_buffer_index);
				}

				ASSERT(inst->ra_temp_buffer_index == next_piece);

				/* if this is the first read ahead piece only increace
				 * the readahead position by the amount that counts */
				if ((inst->ra_pos - inst->pos) == 0) {
					const int offset = (inst->ra_pos + inst->offset) - (next_piece * inst->block_size);
					inst->ra_pos += sz - offset;
				} else {
					inst->ra_pos += sz;
				}
			} else {
				/* DEBUG_VPRINT(LOG_MODULE, "Piece %i dropped (requested %i)",
					inst->ra_temp_buffer_index, next_piece); */
			}

			inst->ra_temp_buffer = nullptr;
			pthread_cond_signal(&inst->ra_cond);
			pthread_mutex_unlock(&inst->lock);
			continue;
		} else {
			/* wait until the next piece is downloaded */
			pthread_cond_wait(&inst->ra_cond, &inst->lock);
			pthread_cond_signal(&inst->ra_cond);
			pthread_mutex_unlock(&inst->lock);
			continue;
		}
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
	while (inst->blocking) {
		pthread_cond_broadcast(&inst->ra_cond);
		pthread_mutex_unlock(&inst->lock);
		usleep(50LL * 1000LL);
		pthread_mutex_lock(&inst->lock);
	}

	pthread_mutex_unlock(&inst->lock);

	/* if this is a stream then wait for the readahead worker */
	if (inst->flags & AVBOX_TORRENTFLAGS_STREAM) {
		DEBUG_PRINT(LOG_MODULE, "Waiting for readahead thread to quit...");
		ASSERT(inst->ra_worker != nullptr);
		pthread_cond_broadcast(&inst->ra_cond);
		pthread_cond_signal(&inst->ra_piece_read_cond);
		avbox_delegate_wait(inst->ra_worker, nullptr);
		inst->ra_worker = nullptr;
		avbox_thread_destroy(inst->ra_thread);

		/* delete all cached pieces */
		while (!inst->ra_pieces.empty()) {
			struct cache_entry * const entry =
				inst->ra_pieces.front();
			inst->ra_pieces.pop();
			delete entry;
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
	ASSERT(inst->ra_thread != nullptr);

	pthread_mutex_lock(&inst->lock);
	inst->blocking++;

	/* if the stream is closed return error */
	if (inst->closed) {
		inst->blocking--;
		pthread_mutex_unlock(&inst->lock);
		errno = ESHUTDOWN;
		return -1;
	}

	/* if we're trying to read beyond the EOF return
	 * 0 */
	if (inst->have_metadata && inst->pos >= inst->filesize) {
		DEBUG_VPRINT(LOG_MODULE, "EOF reached (pos=%" PRIi64 " filesize=%" PRIi64 ")",
			inst->pos, inst->filesize);
		inst->blocking--;
		pthread_mutex_unlock(&inst->lock);
		return 0;
	}


	int64_t ra_min = MIN(inst->filesize - inst->pos, inst->download_ahead_min);
	int64_t ra_avail = inst->ra_pos - inst->pos;

	/* if we don't have enough bytes in ra wait */
	if (!inst->have_metadata || !inst->warm || (inst->underrun && (ra_avail < ra_min)) || (ra_avail == 0)) {
		if (inst->warm && (ra_avail == 0)) {
			inst->underrun = 1;
		}
		pthread_cond_wait(&inst->ra_cond, &inst->lock);

		ra_min = MIN(inst->filesize - inst->pos, inst->download_ahead_min);
		ra_avail = inst->ra_pos - inst->pos;

		if (!inst->have_metadata || !inst->warm || (inst->underrun && (ra_avail < ra_min)) || (ra_avail == 0)) {
			inst->blocking--;
			pthread_mutex_unlock(&inst->lock);
			errno = EAGAIN;
			return -1;
		}
	}

	/* if we're trying to read beyond the EOF return
	 * 0 */
	if (inst->pos >= inst->filesize) {
		DEBUG_VPRINT(LOG_MODULE, "EOF reached (pos=%" PRIi64 " filesize=%" PRIi64 ")",
			inst->pos, inst->filesize);
		inst->blocking--;
		pthread_mutex_unlock(&inst->lock);
		return 0;
	}


	const int next_piece = offset_to_piece_index(inst, inst->pos);

	/* get the next piece from the cache */
	if (inst->ra_current_piece != next_piece) {
		/* DEBUG_VPRINT(LOG_MODULE, "Getting next piece %i", next_piece); */
		struct cache_entry *entry;
		ASSERT(!inst->ra_pieces.empty());
		entry = inst->ra_pieces.front();
		ASSERT(entry->index == next_piece);
		inst->ra_current_piece = next_piece;
		inst->ra_current_piece_size = entry->size;
		inst->ra_current_piece_buffer = entry->buffer;
		inst->ra_pieces.pop();
		delete entry;
	}

	/* copy the bytes requested */
	const int offset = (inst->pos + inst->offset) - (next_piece * inst->block_size);
	const int bytes_to_read = MIN(sz, inst->ra_current_piece_size - offset);
	memcpy(buf, &inst->ra_current_piece_buffer[offset], bytes_to_read);

	/* DEBUG_VPRINT(LOG_MODULE, "Read %i bytes from piece %i at offset %i (pos=%" PRIi64 " offset=%" PRIi64 " piece_size=%" PRIi64
		"ra_pos=%" PRIi64 " count=%" PRIi64 ")",
		bytes_to_read, next_piece, offset, inst->pos, inst->offset, inst->block_size,
		inst->ra_pos, inst->ra_pos - inst->pos); */

	inst->blocking--;
	inst->pos += bytes_to_read;
	inst->underrun = 0;
	pthread_cond_signal(&inst->ra_cond);
	pthread_mutex_unlock(&inst->lock);

	return bytes_to_read;
}


EXPORT void
avbox_torrent_bufferstate(struct avbox_torrent * const inst,
	int64_t * const count, int64_t * const capacity)
{
	if (!inst->warm) {
		*capacity = inst->warmup_pieces * inst->block_size;
		if (*capacity == 0) {
			*capacity = 100;
		}
		*count = inst->warm_pieces * inst->block_size;
		if (*count > *capacity) {
			*count = *capacity;
		}
	} else {
		const int64_t pos = inst->pos;
		const int64_t torrent_pos = inst->torrent_pos;

		*count = *capacity = inst->download_ahead_min;

		if (torrent_pos < inst->filesize &&
			(pos + inst->download_ahead_min) < inst->filesize) {
			const int next_piece = offset_to_piece_index(inst, torrent_pos);
			const int last_piece = offset_to_piece_index(inst,
				MIN(inst->filesize, pos + inst->download_ahead_min));
			*count = MIN(inst->download_ahead_min, torrent_pos - pos);
			for (int i = next_piece; i < last_piece; i++) {
				if (inst->handle.have_piece(i)) {
					*count += inst->block_size;
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
	if (inst->handle.is_valid() && inst->have_metadata) {
		lt::torrent_status st = inst->handle.status();
		return st.total_done;
	}
	return 0;
}


EXPORT int64_t
avbox_torrent_tell(struct avbox_torrent * const inst)
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
	while (!inst->ra_pieces.empty()) {
		struct cache_entry * const entry =
			inst->ra_pieces.front();
		inst->ra_pieces.pop();
		delete entry;
	}

	/* update the position and priorities */
	inst->pos = inst->torrent_pos = inst->ra_pos = pos;
	update_torrent_pos(inst);
	adjust_priorities(inst);

	/* Clear the first piece. This is necessary because if we
	 * are currently at piece X and we seek to piece Y, don't read
	 * anything and then return to piece X piece 8 will still be the
	 * current piece but the readahead thread won't know about it so it
	 * will still place piece 8 in the readahead queue */
	inst->ra_current_piece = -1;
	inst->ra_current_piece_buffer = nullptr;

	/* seeking has the same effect as reading as far as the
	 * readahead thread is concerned so we just signal this
	 * condition here */
	pthread_cond_signal(&inst->ra_cond);

	DEBUG_VPRINT(LOG_MODULE, "Returning from seek: %" PRIi64,
		inst->pos);

	pthread_mutex_unlock(&inst->lock);


	return 0;
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

	/* initialize pthread primitives */
	if (pthread_mutex_init(&inst->lock, nullptr) != 0) {
		ABORT("Could not initialize pthread primitives!");
	}

	if (pthread_cond_init(&inst->ra_piece_read_cond, nullptr) != 0 ||
		pthread_cond_init(&inst->ra_cond, nullptr) != 0) {
		ABORT("Could not initialize pthread primitives!");
	}

	/* initialize stream */
	inst->pos = inst->torrent_pos = 0;
	inst->underrun = 0;
	inst->block_size = -1;
	inst->n_pieces = 0;
	inst->flags = flags;
	inst->notify_object = notify_object;

	/* add the torrent to the session */
	if (torrent_filename != "") {
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
	pthread_mutex_lock(&session_lock);
	LIST_ADD(&torrents, inst);
	inst->handle = session->add_torrent(params, boost::ref(ec));
	if (ec) {
		LOG_VPRINT_ERROR(LOG_MODULE, "Could not add torrent: %s",
			ec.message().c_str());
		LIST_REMOVE(inst);
		free(inst);
		if (!torrent_filename.empty()) {
			unlink(torrent_filename.c_str());
		}
		pthread_mutex_unlock(&session_lock);
		return NULL;
	}
	pthread_mutex_unlock(&session_lock);

	/* save info hash */
	inst->info_hash = lt::to_hex(inst->handle.info_hash().to_string());

	if (torrent_filename != "") {
		unlink(torrent_filename.c_str());
		metadata_received(inst);
	}

	DEBUG_VPRINT(LOG_MODULE, "Torrent added: info_hash=%s",
		inst->info_hash.c_str());


	/* initialize readahead struff now */
	inst->ra_pos = 0;
	inst->ra_worker = nullptr;
	inst->ra_current_piece = -1;
	inst->ra_current_piece_size = 0;
	inst->ra_current_piece_buffer = nullptr;

	if ((inst->ra_thread = avbox_thread_new(NULL, NULL, AVBOX_THREAD_REALTIME, -5)) == nullptr) {
		ABORT("Could not initialize readahead thread!");
	}

	if (flags & AVBOX_TORRENTFLAGS_STREAM) {
		DEBUG_PRINT(LOG_MODULE, "Starting readahead thread");
		if ((inst->ra_worker = avbox_thread_delegate(inst->ra_thread, readahead_thread, inst)) == nullptr) {
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
	struct avbox_delegate *del;

	DEBUG_PRINT(LOG_MODULE, "Creating session " STRINGIZE(LOCALSTATEDIR));

#ifdef ENABLE_REALTIME
	/* Use the lowest realtime priority.
	 * NOTE: At first I assumed that using a SCHED_OTHER or SCHED_IDLE policy
	 * would suffice for this since everything works well while not streaming.
	 * However if we don't use a realtime priority it will cause the libtorrent
	 * usermode theads to be starved by their own IO done on kernel threads (kworker),
	 * this eventually causes the realtime threads to be starved from data leading
	 * to interruptions */
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
		lt::alert::status_notification);

#if 0
	/* enable uTP and TCP and uTP only for outgoing */
	settings.set_bool(lt::settings_pack::enable_outgoing_utp, true);
	settings.set_bool(lt::settings_pack::enable_outgoing_tcp, true);
	settings.set_bool(lt::settings_pack::enable_incoming_utp, true);
	settings.set_bool(lt::settings_pack::enable_incoming_tcp, true);

	settings.set_int(lt::settings_pack::download_rate_limit, 0);
	settings.set_int(lt::settings_pack::upload_rate_limit, 512 * 1024);
	settings.set_int(lt::settings_pack::aio_threads, 1);
	settings.set_int(lt::settings_pack::aio_max, 10);
	settings.set_int(lt::settings_pack::max_queued_disk_bytes, 1024 * 32);
	settings.set_int(lt::settings_pack::coalesce_reads, true);
	settings.set_int(lt::settings_pack::coalesce_writes, true);
#endif

	if ((session = new lt::session(settings)) == nullptr) {
		DEBUG_PRINT(LOG_MODULE, "Could not create libtorrent session!");
		return -1;
	}

	lt::peer_class_type_filter peer_classes;
	peer_classes.add(lt::peer_class_type_filter::tcp_socket, lt::session::global_peer_class_id);
	peer_classes.add(lt::peer_class_type_filter::ssl_tcp_socket, lt::session::global_peer_class_id);
	peer_classes.add(lt::peer_class_type_filter::i2p_socket, lt::session::global_peer_class_id);
	peer_classes.add(lt::peer_class_type_filter::utp_socket, lt::session::global_peer_class_id);
	peer_classes.add(lt::peer_class_type_filter::ssl_utp_socket, lt::session::global_peer_class_id);
	session->set_peer_class_type_filter(peer_classes);


	/* create a thread to monitor the session */
	if ((thread = avbox_thread_new(NULL, NULL, AVBOX_THREAD_REALTIME, -5)) == nullptr) {
		DEBUG_VPRINT(LOG_MODULE, "Could not create thread: %s",
			strerror(errno));
		return -1;
	}
	if ((del = avbox_thread_delegate(thread, session_monitor, NULL)) == nullptr) {
		ABORT("Could not create torrent thread!");
	}

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
		avbox_thread_destroy(thread);
		delete session;
		session = nullptr;
		quit = 0;
	}
}
