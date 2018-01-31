/**
 * MediaBox - Linux based set-top firmware
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#define LOG_MODULE "download-manager"

#include "lib/avbox.h"
#include "downloads-backend.h"

#define PREFIX "/usr/local"

#define STRINGIZE2(x)	#x
#define STRINGIZE(x)	STRINGIZE2(x)


#define MBOX_DOWNLOADTYPE_NONE		(0)
#define MBOX_DOWNLOADTYPE_TORRENT	(1)


struct mbox_dlman_download_item*
mbox_dlman_next(struct mbox_dlman_download_item * const current)
{
	ASSERT(current != NULL);

	if (current->type == MBOX_DOWNLOADTYPE_NONE ||
		current->type == MBOX_DOWNLOADTYPE_TORRENT) {
		struct avbox_torrent * next = avbox_torrent_next(current->stream);
		if (next == NULL) {
			return NULL;
		} else {
			int64_t total_bytes, downloaded_bytes;
			current->stream = next;
			current->id = avbox_torrent_id(next);
			current->name = avbox_torrent_name(next);
			current->percent = 0;
			if ((total_bytes = avbox_torrent_size(next)) != -1) {
				downloaded_bytes = avbox_torrent_downloaded(next);
				current->percent = (((downloaded_bytes * 100) / total_bytes) * 100) / 100;
			}
			return current;
		}
	} else {
		ABORT("Invalid download type!");
	}
}

void
mbox_dlman_item_unref(struct mbox_dlman_download_item * const inst)
{
	avbox_torrent_unref(inst->stream);
}


/**
 * Adds a URL to the download queue.
 */
int
mbox_dlman_addurl(const char * const url)
{
	if (!strncmp(url, "magnet:", 7)) {
		struct avbox_torrent *torrent;
		if ((torrent = avbox_torrent_open(url, NULL,
			AVBOX_TORRENTFLAGS_NONE, NULL)) == NULL) {
			LOG_VPRINT_ERROR("Could not open torrent stream (%s): %s",
				url, strerror(errno));
			return -1;
		}
		avbox_torrent_moveonfinish(torrent, STRINGIZE(LOCALSTATEDIR) "/lib/mediabox/store/Video");
		return 0;
	} else {
		LOG_VPRINT_ERROR("URI scheme not supported: %s",
			url);
		errno = ENOTSUP;
		return -1;
	}
}


/**
 * Initialize the download manager.
 */
int
mb_downloadmanager_init(void)
{
	return 0;
}


/**
 * Shutdown the download manager.
 */
void
mb_downloadmanager_destroy(void)
{
	DEBUG_PRINT("download-backend", "Shutting down download manager");
}
