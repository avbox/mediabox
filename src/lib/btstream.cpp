#include <pthread.h>
#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/settings.hpp>


#define LOG_MODULE "btstream"

#include "avbox.h"


struct avbox_btstream
{
	char *uri;
	static libtorrent::session session;
	libtorrent::torrent_handle handle;
};

static int session_refs = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;


static void *
session_run(void *data)
{
	libtorrent::torrent_handle torrent;

	while (1) {
		std::vector<libtorrent::alert*> alerts;
		//session.pop_alerts(&alerts);

		for (libtorrent::alert const * a : alerts) {

			if (auto alert = libtorrent::alert_cast<libtorrent::add_torrent_alert>(a)) {
				torrent = alert->handle;
			}

			if (auto alert = libtorrent::alert_cast<libtorrent::torrent_finished_alert>(a)) {




			}
		}
	}
	return NULL;
}


static void
session_shutdown(void)
{
	pthread_mutex_lock(&lock);
	if (--session_refs == 0) {
		/* destroy session */
	}
	pthread_mutex_unlock(&lock);
}


static int
session_init(void)
{
	int ret = -1;
	pthread_mutex_lock(&lock);
	if (session_refs == 0) {
		/* start the session */
	}
	session_refs++;
	ret = 0;
	pthread_mutex_unlock(&lock);
	return ret;
}


void
avbox_btstream_close(struct avbox_btstream * const inst)
{
	session_shutdown();
	free(inst);
}


struct avbox_btstream*
avbox_btstream_open(const char *uri)
{
	struct avbox_btstream *inst;
	libtorrent::add_torrent_params params;
	libtorrent::settings_pack settings;

	settings.set_int(libtorrent::settings_pack::alert_mask,
		libtorrent::alert::error_notification |
		libtorrent::alert::storage_notification |
		libtorrent::alert::status_notification);
	
	libtorrent::session session(settings);

	if (session_init() == -1) {
		LOG_PRINT_ERROR("Could not start libtorrent session!");
		return NULL;
	}

	if ((inst = (struct avbox_btstream*) malloc(sizeof(struct avbox_btstream))) == NULL) {
		ASSERT(errno == ENOMEM);
		session_shutdown();
		return NULL;
	}

	if ((inst->uri = strdup(uri)) == NULL) {
		ASSERT(errno == ENOMEM);
		session_shutdown();
		free(inst);
		return NULL;
	}

	params.url = inst->uri;
	params.save_path = "/data/Downloads";
	inst->handle = session.add_torrent(params);

	return inst;
}
