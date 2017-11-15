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


#ifndef __MBOX_LIBRARY_H__
#define __MBOX_LIBRARY_H__

#include <dirent.h>
#include <sqlite3.h>

#include "lib/linkedlist.h"


LISTABLE_STRUCT(mbox_library_dirent,
	int isdir;
	char *path;
	char *name;
);


struct mbox_library_rootdir
{
	struct mbox_library_dirent *ptr;
	LIST entries;
};

struct mbox_library_localdir
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int dotdot_sent;
};

struct mbox_library_upnpdir
{
	DIR *dir;
	char *path;
};

#ifdef ENABLE_BLUETOOTH
struct mbox_library_btdir
{
	struct avbox_btdev **devs;
	struct avbox_btdev **cur;
	int read;
};
#endif

struct mbox_library_emptydir
{
	int read;
};


struct mbox_library_dir
{
	int type;
	char *path;
	union {
		struct mbox_library_upnpdir upnpdir;
		struct mbox_library_rootdir rootdir;
		struct mbox_library_localdir localdir;
#ifdef ENABLE_BLUETOOTH
		struct mbox_library_btdir btdir;
#endif
		struct mbox_library_emptydir emptydir;
	} state;
};


/**
 * Open a library directory
 */
struct mbox_library_dir *
mbox_library_opendir(const char * const path);


/**
 * Read the next entry in an open directory.
 */
struct mbox_library_dirent *
mbox_library_readdir(struct mbox_library_dir * const dir);


/**
 * Free the directory entry.
 */
void
mbox_library_freedirentry(struct mbox_library_dirent * const ent);


/**
 * Close a library directory.
 */
void
mbox_library_closedir(struct mbox_library_dir * const dir);


/**
 * Gets the list of watched directories.
 */
const char **
mbox_library_watchdirs(void);


/**
 * Initialize the library backend.
 */
int
mbox_library_init(void);


/**
 * Shutdown the library backend.
 */
void
mbox_library_shutdown(void);


#endif
