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
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <assert.h>
#include <limits.h>
#include <sqlite3.h>
#include <magic.h>
#include <sys/mount.h>
#include <sys/inotify.h>
#include <regex.h>
#include <pthread.h>
#include <signal.h>

#define LOG_MODULE "library"

#include "lib/log.h"
#include "lib/debug.h"
#include "lib/process.h"
#include "lib/iface_util.h"
#include "lib/linkedlist.h"
#include "lib/proc_util.h"
#include "lib/file_util.h"
#include "lib/db_util.h"
#include "library.h"
#include "lib/delegate.h"
#include "lib/thread.h"
#include "lib/string_util.h"
#include "lib/bluetooth.h"
#include "lib/application.h"



#define STRINGIZE2(x)	#x
#define STRINGIZE(x)	STRINGIZE2(x)


#define MEDIATOMB_BIN		"/usr/bin/mediatomb"
#define MEDIATOMB_RUN		"/tmp/mediabox/mediatomb"
#define MEDIATOMB_VAR 		STRINGIZE(LOCALSTATEDIR) "/lib/mediabox/mediatomb"

#define FUSERMOUNT_BIN		"/usr/bin/fusermount"
#define AVMOUNT_BIN		"/usr/bin/avmount"
#define AVMOUNT_MOUNTPOINT	"/media/UPnP"
#define UPNP_ROOT		"/media/UPnP"
#define DEFAULT_LOGFILE		STRINGIZE(LOCALSTATEDIR) "/log/avmount-mediabox.log"

#define MBOX_STORE_MOUNTPOINT	STRINGIZE(LOCALSTATEDIR) "/lib/mediabox/store"
#define MBOX_STORE_VIDEO	STRINGIZE(LOCALSTATEDIR) "/lib/mediabox/store/Video"
#define MBOX_STORE_AUDIO	STRINGIZE(LOCALSTATEDIR) "/lib/mediabox/store/Audio"


#define MBOX_LIBRARY_LOCAL_DIRECTORY_AUDIO	(1)
#define MBOX_LIBRARY_LOCAL_DIRECTORY_MOVIES	(3)
#define MBOX_LIBRARY_LOCAL_DIRECTORY_SERIES	(4)


#define MBOX_LIBRARY_DIRTYPE_ROOT 	(0)
#define MBOX_LIBRARY_DIRTYPE_LOCAL	(1)
#define MBOX_LIBRARY_DIRTYPE_UPNP	(2)
#define MBOX_LIBRARY_DIRTYPE_DVD	(3)
#define MBOX_LIBRARY_DIRTYPE_BLUETOOTH	(4)
#define MBOX_LIBRARY_DIRTYPE_TV		(5)

LISTABLE_STRUCT(mb_mediatomb_inst,
	int procid;
);


struct mt_init_state
{
	int port;
	int err;
	int gotone;
};


LISTABLE_STRUCT(mbox_library_local_watchdir,
	int watch_fd;
	char *path;
);


static char * mediatomb_home = NULL;
static LIST mediatomb_instances;

static int avmount_process_id = -1;
static int local_inotify_fd = -1;
static int local_inotify_quit = 0;
static char *store;
static pthread_t local_inotify_thread;
static LIST local_inotify_watches;

#if defined(ENABLE_DVD) || defined(ENABLE_USB)
static struct udev *udev = NULL;
#endif


#if 0
static int
mbox_library_mediatombadd(const char * const path)
{
	int process_id, exit_status;
	const char * args[] =
	{
		MEDIATOMB_BIN,
		"--config",
		MEDIATOMB_RUN "/config.xml",
		"--home",
		mediatomb_home,
		"--add",
		path,
		NULL
	};

	/* launch the mediatomb process */
	if ((process_id = avbox_process_start(MEDIATOMB_BIN, (const char **) args,
		AVBOX_PROCESS_NICE | AVBOX_PROCESS_IONICE_IDLE |
		AVBOX_PROCESS_SUPERUSER | AVBOX_PROCESS_WAIT |
		AVBOX_PROCESS_STDOUT_LOG | AVBOX_PROCESS_STDERR_LOG,
		"mediatomb-add", NULL, NULL)) == -1) {
		LOG_VPRINT_ERROR("Could not execute deluge-console (errno=%i)",
			errno);
		return -1;
	}

	/* wait for process to exit */
	avbox_process_wait(process_id, &exit_status);

	return exit_status;
}
#endif


/**
 * Starts a mediatomb instance.
 */
static int
mb_library_backend_startmediatomb(const char * iface_name, void  *data)
{
	struct mb_mediatomb_inst *inst;
	struct mt_init_state *state = (struct mt_init_state*) data;
	char port[6];
	char homedir[128];
	char * mtargs[] =
	{
		MEDIATOMB_BIN,
		"-e",
		(char*) iface_name,
		"--port",
		port,
		"--config",
		MEDIATOMB_RUN "/config.xml",
		"--home",
		homedir,
		"--add",
		MBOX_STORE_VIDEO,
		NULL
	};

	assert(data != NULL);
	assert(state->port > 0 && state->port < 65536);

	/* for now stop after the first instance is
	 * launched */
	if (!strcmp(iface_name, "lo") || state->gotone) {
		return 0;
	} else {
		state->gotone = 1;
	}

	/* if we errored out on a previous iteration
	 * then exit */
	if (state->err != 0) {
		return 0;
	}

	/* fill the dynamic arguments (port # and homedir) as
	 * every mt instance must run in it's own home directory
	 * and port. Also create the home directory if it doesn't
	 * already exists */
	snprintf(port, sizeof(port), "%i", state->port++);
	snprintf(homedir, sizeof(homedir), "%s", mediatomb_home);
	if (mkdir_p(homedir, S_IRWXU | S_IRWXG) == -1) {
		state->err = 1;
		return 0;
	}

	/* for now don't launch loopback instance */
	if (!strcmp("lo", iface_name)) {
		mtargs[6] = MEDIATOMB_RUN "/config-local.xml";
	}

	if ((inst = malloc(sizeof(struct mb_mediatomb_inst))) == NULL) {
		LOG_VPRINT_ERROR("Could not allocate memory for mediatomb instance %s",
			iface_name);
		return -1;
	}

	/* launch the mediatomb process */
	if ((inst->procid = avbox_process_start(MEDIATOMB_BIN, (const char **) mtargs,
		AVBOX_PROCESS_AUTORESTART | AVBOX_PROCESS_NICE | AVBOX_PROCESS_IONICE_IDLE |
		AVBOX_PROCESS_SUPERUSER /*| AVBOX_PROCESS_STDOUT_LOG | AVBOX_PROCESS_STDERR_LOG*/,
		"mediatomb", NULL, NULL)) == -1) {
		LOG_PRINT_ERROR("Could not start mediatomb daemon");
		free(inst);
		return -1;
	}

	LIST_ADD(&mediatomb_instances, inst);

	return 0;
}


/**
 * Copies a mediatomb config file to the runtime
 * directory.
 */
static int
mb_library_backend_configcp(const char * const template_path,
	const char * const filename)
{
	char src_path[255];
	char dst_path[255];

	snprintf(src_path, sizeof(src_path), "%s/%s", template_path, filename);
	snprintf(dst_path, sizeof(dst_path), MEDIATOMB_RUN "/%s", filename);

	/* copy the file */
	if (cp(src_path, dst_path) == -1) {
		LOG_VPRINT(LOGLEVEL_ERROR, "library-backend",
			"Could not copy '%s' to '%s'!", src_path, dst_path);
		return -1;
	}

	return 0;
}


/**
 * Write a random generate UUID string to the
 * buffer. The buffer needs to be 37 characters
 * long.
 */
static char *
getuuidstring(char * const buf)
{
	int fd;
	assert(buf != NULL);
	if ((fd = open("/proc/sys/kernel/random/uuid", O_RDONLY, NULL)) == -1) {
		LOG_VPRINT_ERROR("Could not open '/proc/sys/kernel/random/uuid': %s",
			strerror(errno));
		return NULL;
	}
	if (read(fd, buf, 36) != 36) {
		LOG_VPRINT_ERROR("Could not read uuid!: %s",
			strerror(errno));
		close(fd);
		return NULL;
	}
	close(fd);
	buf[36] = '\0';
	return buf;
}


/**
 * Initialize mediabox config files
 */
static char *
mb_library_backend_mediaboxsetup(const char * const template_path)
{
	int fd;
	char * ret = NULL;
	char * udnfile = NULL;
	char * mediatomb_home = NULL;
	char mediatomb_udn[37] = "", hostname[36] = "";

	DEBUG_VPRINT("library-backend", "Mediatomb setup from: %s",
		template_path);

	/* create mediatomb runtime directory */
	if (mkdir_p(MEDIATOMB_RUN, S_IRWXU | S_IRWXG) == -1) {
		LOG_PRINT_ERROR("Could not create mediatomb runtime directory!");
		goto end;
	}

	/* create the home directory */
	if (mkdir_p(MEDIATOMB_VAR, S_IRWXU | S_IRWXG) == -1) {
		LOG_PRINT_ERROR("Could not create mediatomb variable directory!");
	} else {
		if ((mediatomb_home = strdup(MEDIATOMB_VAR)) == NULL) {
			LOG_PRINT_ERROR("Could not allocate memory for homedir string!!");
		}
	}

	/* if the home directory is unaccessible use the user's
	 * home directory */
	if (mediatomb_home == NULL) {
		#define CONFIGDIR "/.mediabox/mediatomb"

		char * const homedir = getenv("HOME");
		char * configdir = NULL;

		if (homedir != NULL) {
			if ((configdir = malloc(strlen(homedir) + sizeof(CONFIGDIR))) == NULL) {
				LOG_PRINT_ERROR("Could not allocate memory for homedir string!!!");
			} else {
				strcpy(configdir, homedir);
				strcat(configdir, CONFIGDIR);
				DEBUG_VPRINT("library-backend", "Attempting to create '%s'",
					configdir);
				if (mkdir_p(configdir, S_IRWXU | S_IRWXG) == -1) {
					LOG_PRINT_ERROR("Could not create mediatomb variable directory!!");
					free(configdir);
				} else {
					mediatomb_home = configdir;
				}
			}
		}
	}

	/* if at this point we didn't find a home directory
	 * then we're out of luck */
	if (mediatomb_home == NULL) {
		LOG_PRINT_ERROR("Could not find a suitable home directory!");
		goto end;
	}

	DEBUG_VPRINT("library-backend", "Using mediatomb homedir: %s",
		mediatomb_home);

	/* copy config files */
	if (mb_library_backend_configcp(template_path, "config.xml") != 0 ||
		mb_library_backend_configcp(template_path, "common.js") != 0 ||
		mb_library_backend_configcp(template_path, "import.js") != 0 ||
		mb_library_backend_configcp(template_path, "playlists.js") != 0) {
		goto end;
	}

	/* compose udn filepath */
	if ((udnfile = malloc((strlen(mediatomb_home) + 5) * sizeof(char))) == NULL) {
		LOG_PRINT_ERROR("Could not allocate memory for udn filepath!");
		goto end;
	}
	strcpy(udnfile, mediatomb_home);
	strcat(udnfile, "/udn");

	DEBUG_VPRINT("library-backend", "Attempting to open %s",
		udnfile);

	/* try to read the udn file (if it exists) */
	if ((fd = open(udnfile, O_RDONLY, NULL)) == -1) {
		if (errno != ENOENT) {
			LOG_VPRINT_ERROR("Could not open %s for read access: %s!",
				udnfile, strerror(errno));
			goto end;
		}
	} else {
		if (read(fd, mediatomb_udn, 36) == -1) {
			LOG_VPRINT_ERROR("Could not read %s: %s!!",
				udnfile, strerror(errno));
			goto end;
		}
		close(fd);
	}

	/* if there's no udn file generate one */
	if (*mediatomb_udn == '\0') {
		DEBUG_VPRINT("library-backend", "Creating %s",
			udnfile);
		if (getuuidstring(mediatomb_udn) == NULL) {
			LOG_PRINT_ERROR("Could not generate UUID!!");
			goto end;
		}
		DEBUG_VPRINT("library-backend", "New uuid: %s",
			mediatomb_udn);
		if ((fd = open(udnfile, O_CREAT | O_RDWR, S_IRWXU | S_IRWXG)) == -1) {
			LOG_VPRINT_ERROR("Could not create '%s': %s!",
				udnfile, strerror(errno));
			goto end;
		}
		assert(strlen(mediatomb_udn) == 36);
		if (write(fd, mediatomb_udn, 36) == -1) {
			LOG_VPRINT_ERROR("Could not save udn file %s. Continuing.",
				udnfile);
		}
		close(fd);
	}

	/* get the system's hostname */
	if (gethostname(hostname, sizeof(hostname)) == -1) {
		LOG_VPRINT_ERROR("Could not get hostname: %s",
			strerror(errno));
	}

	DEBUG_VPRINT("library-backend", "System hostname is '%s'",
		hostname);

	const char * const match[] =
	{
		"@HOMEDIR@",
		"@UDN@",
		"@HOSTNAME@",
		"@ENABLEUI@",
		NULL
	};
	const char * replace[] =
	{
		mediatomb_home,
		mediatomb_udn,
		hostname,
		"yes",
		NULL
	};

	/* now that we have the configuration files
	 * on our home directory we need to edit them. */
	if (frep(MEDIATOMB_RUN "/config.xml",
		MEDIATOMB_RUN "/config-local.xml",
		match, replace) != 0) {
		LOG_PRINT_ERROR("Could not update config-local.xml!!");
		goto end;
	}

	replace[3] = "yes"; /* @ENABLEUI@ */

	if (frep(MEDIATOMB_RUN "/config.xml",
		MEDIATOMB_RUN "/config.xml",
		match, replace) != 0) {
		LOG_PRINT_ERROR("Could not update config.xml!!");
		goto end;
	}

	ret = mediatomb_home;
end:
	if (udnfile != NULL) {
		free(udnfile);
	}
	if (ret == NULL && mediatomb_home != NULL) {
		free(mediatomb_home);
	}
	return ret;
}


/**
 * Makes sure to unmount the /media/UPnP mountpoint before
 * restarting avmount if it crashes.
 */
static int
avbox_avmount_exit(int id, int exit_status, void *data)
{
	DEBUG_PRINT("library-backend", "Running fusermount");
	(void) data;
	(void) exit_status;
	if (avbox_execargs(FUSERMOUNT_BIN, "-u", "/media/UPnP", NULL) != 0) {
		LOG_VPRINT_ERROR("Could not unmount /media/UPnP: '%s' failed!",
			FUSERMOUNT_BIN);
	}
	/* we can return non-zero here to stop autorestart */
	return 0;
}


static struct avbox_library_dirent *
mbox_library_adddirent(const char * const name,
	const char * const path, int isdir, LIST *list)
{
	struct mbox_library_dirent *ent;

	if ((ent = malloc(sizeof(struct mbox_library_dirent))) == NULL) {
		return NULL;
	}

	ent->isdir = isdir;
	ent->name = NULL;
	ent->path = NULL;

	if (name != NULL) {
		if ((ent->name = strdup(name)) == NULL) {
			free(ent);
			return NULL;
		}
	}

	if (path != NULL) {
		if ((ent->path = strdup(path)) == NULL) {
			free(ent->name);
			free(ent);
			return NULL;
		}
	}

	LIST_APPEND(list, ent);

	return NULL;
}


/**
 * Strip a file extension IN PLACE
 * and return a pointer to the extension.
 */
static char *
mbox_library_stripext(char *filename)
{
	char *p;

	ASSERT(filename != NULL);

	if (filename[0] == '.' && filename[1] == '.' && filename[2] == '\0') {
		return NULL;
	}

	p = filename + strlen(filename);
	ASSERT(*p == '\0');

	while (p != filename && *p != '.') {
		p--;
	}

	if (*p == '.') {
		*p = '\0';
		return ++p;
	}
	return NULL;
}


static char *
mbox_library_striplastlevel(char * const path)
{
	char *ppath, *newpath;
	if ((newpath = strdup(path)) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}

	/* strip the last level from the path */
	ppath = newpath + strlen(newpath) - 1;
	if (*ppath == '/') {
		ppath--;
	}
	while (*ppath != '/') {
		ppath--;
	}
	*++ppath = '\0';
	return newpath;
}


static char *
mbox_library_transform_video_title(char * title)
{
	char *tmp;

	title = strreplace(title, "\t", "");
	title = strreplace(title, "YIFY", "");
	title = strreplace(title, "BluRay", "");
	title = strreplace(title, "x264", "");
	title = strreplace(title, "BrRip", "");
	title = strreplace(title, "HDRip", "");
	title = strreplace(title, "AAC2", "");
	title = strreplace(title, "AAC-JYK", "");
	title = strreplace(title, "bitloks", "");
	title = strreplace(title, "H264", "");
	title = strreplace(title, "AAC-RARBG", "");
	title = strreplace(title, "SiNNERS", "");
	title = strreplace(title, "X264", "");
	title = strreplace(title, "XViD", "");
	title = strreplace(title, "XviD", "");
	title = strreplace(title, "EVO", "");
	title = strreplace(title, "_", " ");
	title = strreplace(title, "psig", " ");
	title = strreplace(title, "xvid", "");
	title = strreplace(title, "dvdrip", "");
	title = strreplace(title, "ac3", "");
	title = strreplace(title, "AC3", "");
	title = strreplace(title, "DvDrip", "");
	title = strreplace(title, "DVDRip", "");
	title = strreplace(title, "internal", "");
	title = strreplace(title, "iNFAMOUS", "");
	title = strreplace(title, "HD-CAM", "");
	title = strreplace(title, "AC3-CPG", "");
	title = strreplace(title, "HQMic", "");
	title = strreplace(title, "BRRip", "");
	title = strreplace(title, "Bluray", "");
	title = strreplace(title, "500MB", "");
	title = strreplace(title, "aXXo", "");
	title = strreplace(title, "VPPV", "");
	title = strreplace(title, "BOKUTOX", "");
	title = strreplace(title, "George Lucas", "");
	title = strreplace(title, "Eng Subs", "");
	title = strreplace(title, "BRrip", "");
	title = strreplace(title, "DTS", "");
	title = strreplace(title, "GAZ", "");
	title = strreplace(title, "AAC", "");
	title = strreplace(title, "YTS", "");
	title = strreplace(title, "AG", "");
	title = strreplace(title, "RARBG", "");
	title = strreplace(title, "CPG", "");
	title = strreplace(title, "HD TS", "");
	title = strreplace(title, "SyED", "");
	title = strreplace(title, "MkvCage", "");
	title = strreplace(title, "WEBRip", "");
	title = strreplace(title, "HC ETRG", "");
	title = strreplace(title, "DVDSrc", "");
	title = strreplace(title, "XVID", "");
	title = strreplace(title, "HQ Hive", "");
	title = strreplace(title, "CM8", "");
	title = strreplace(title, "mkv muxed old", "");
	title = strreplace(title, "0 STUTTERSHIT", "");
	title = strreplace(title, "WEB DL", "");
	title = strreplace(title, "JYK", "");
	title = strreplace(title, "Xvid", "");
	title = strreplace(title, "avi", "");
	title = strreplace(title, "mp4", "");
	title = strreplace(title, ".", " ");
	title = strreplace(title, "-", " ");

	do {
		tmp = strdup(title);
		ASSERT(tmp != NULL);
		title = strreplace(title, "  ", " ");
	} while (strcmp(tmp, title));

	if (tmp != NULL) {
		free(tmp);
	}


	title = strreplace(title, "[]", "");
	title = strreplace(title, "[ ]", "");
	title = strtrim(title);

	if (!strcmp(title, "COM") || !strcmp(title, "com")) {
		title[0] = '\0';
	}

	return title;
}


static struct mbox_library_dirent *
mbox_library_dupdirent(struct mbox_library_dirent * const ent)
{
	struct mbox_library_dirent *newent;
	if ((newent = malloc(sizeof(struct mbox_library_dirent))) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}
	if ((newent->name = strdup(ent->name)) == NULL) {
		ASSERT(errno == ENOMEM);
		free(newent);
		return NULL;
	}
	if ((newent->path = strdup(ent->path)) == NULL) {
		ASSERT(errno == ENOMEM);
		free(newent->name);
		free(newent);
		return NULL;
	}
	newent->isdir = ent->isdir;
	return newent;
}


static int
mbox_library_local_open_database(sqlite3 **db, const int flags)
{
	int ret = -1;
	char *filename = NULL;

	if ((filename = avbox_dbutil_getdbfile("content.db")) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}

	/* open db connection */
	if (sqlite3_open_v2(filename, db, flags | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Could not open database '%s': %s",
			filename, sqlite3_errmsg(*db));
		errno = EIO;
		goto end;
	}

	ret = 0;
end:
	if (filename != NULL) {
		free(filename);
	}

	return ret;
}


/**
 * Get the id of a library path if it exists.
 */
static int64_t
mbox_library_local_getid(const char * const path, int64_t start_at)
{
	int64_t ret = -1;
	int res;
	size_t len = 0;
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	const char *ppath;
	char *name = NULL, *pname;
	const char * const sql = "SELECT id FROM local_objects WHERE parent_id = ? AND name = ?";

	ppath = (*path == '/') ?  path + 1 : path;
	while (*ppath != '/' && *ppath != '\0') {
		ppath++;
		len++;
	}

	/* this is the root directory */
	if (len == 0) {
		return start_at;
	}

	if ((name = malloc(len + 1)) == NULL) {
		ASSERT(errno == ENOMEM);
		return -1;
	}

	/* copy name */
	ppath = (*path == '/') ?  path + 1 : path;
	pname = name;
	while (len > 0) {
		*pname++ = *ppath++;
		len--;
	}
	*pname = '\0';

	ASSERT(*ppath == '/' || *ppath == '\0');

	/* open db connection */
	if (mbox_library_local_open_database(&db, SQLITE_OPEN_READONLY) == -1) {
		LOG_VPRINT_ERROR("Could not open database: %s",
			sqlite3_errmsg(db));
		goto end;
	}

	/* prepare the query */
	while ((res = sqlite3_prepare_v2(db, sql, -1, &stmt, 0)) != SQLITE_OK) {
		if (res == SQLITE_LOCKED) {
			usleep(100L * 1000L);
			continue;
		} else {
			errno = EFAULT;
		}
		LOG_VPRINT_ERROR("Could not prepare SQL statement: %s", sql);
		LOG_VPRINT_ERROR("SQL Error: %s", sqlite3_errmsg(db));
		goto end;
	}

	/* bind parameters */
	if (sqlite3_bind_int(stmt, 1, start_at) != SQLITE_OK ||
		sqlite3_bind_text(stmt, 2, name, strlen(name), NULL) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Binding failed: %s", sqlite3_errmsg(db));
		goto end;
	}

	/* execute the query and do it again until
	 * we resolve the path */
	while (1) {
		if ((res = sqlite3_step(stmt)) == SQLITE_ROW) {
			ret = sqlite3_column_int(stmt, 0);
			if (strlen(ppath) > 1) {
				ret = mbox_library_local_getid(ppath, ret);
			}
			break;
		} else if (res == SQLITE_BUSY) {
			usleep(100L * 1000L);
			continue;
		} else if (res == SQLITE_DONE) {
			ret = -1;
			break;
		} else if (res == SQLITE_MISUSE) {
			DEBUG_ABORT(LOG_MODULE, "sqlite misuse");
		} else if (res == SQLITE_ERROR) {
			LOG_VPRINT_ERROR("Sqlite error: %s", sqlite3_errmsg(db));
			ret = -1;
			break;
		} else {
			DEBUG_VPRINT(LOG_MODULE, "Step failed: %i", res);
			ret = -1;
			break;
		}
	}

end:
	if (stmt != NULL) {
		sqlite3_finalize(stmt);
	}
	if (db != NULL) {
		sqlite3_close(db);
	}
	if (name != NULL) {
		free(name);
	}

	return ret;
}


static int64_t
mbox_library_local_getid_by_uri(const char * const uri)
{
	int64_t ret = -1;
	int res;
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	const char * const sql = "SELECT id FROM local_objects WHERE path = ? LIMIT 1";

	/* open db connection */
	if (mbox_library_local_open_database(&db, SQLITE_OPEN_READONLY) == -1) {
		LOG_VPRINT_ERROR("Could not open database: %s",
			sqlite3_errmsg(db));
		goto end;
	}

	/* prepare the query */
	while ((res = sqlite3_prepare_v2(db, sql, -1, &stmt, 0)) != SQLITE_OK) {
		if (res == SQLITE_LOCKED) {
			usleep(100L * 1000L);
			continue;
		} else {
			errno = EFAULT;
		}
		LOG_VPRINT_ERROR("Could not prepare SQL statement: %s", sql);
		LOG_VPRINT_ERROR("SQL Error: %s", sqlite3_errmsg(db));
		goto end;
	}

	/* bind parameters */
	if (sqlite3_bind_text(stmt, 1, uri, strlen(uri), NULL) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Binding failed: %s", sqlite3_errmsg(db));
		goto end;
	}

	/* execute the query and do it again until
	 * we resolve the path */
	while (1) {
		if ((res = sqlite3_step(stmt)) == SQLITE_ROW) {
			ret = sqlite3_column_int(stmt, 0);
			break;
		} else if (res == SQLITE_BUSY) {
			usleep(100L * 1000L);
			continue;
		} else if (res == SQLITE_DONE) {
			ret = -1;
			break;
		} else if (res == SQLITE_MISUSE) {
			DEBUG_ABORT(LOG_MODULE, "sqlite misuse");
		} else if (res == SQLITE_ERROR) {
			LOG_VPRINT_ERROR("Sqlite error: %s", sqlite3_errmsg(db));
			ret = -1;
			break;
		} else {
			DEBUG_VPRINT(LOG_MODULE, "Step failed: %i", res);
			ret = -1;
			break;
		}
	}

end:
	if (stmt != NULL) {
		sqlite3_finalize(stmt);
	}
	if (db != NULL) {
		sqlite3_close(db);
	}

	return ret;

}


static int64_t
mbox_library_local_mkdir(const char * const name, const int64_t parent_id)
{
	int res, ret = -1;
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	const char * const sql = "INSERT INTO local_objects (parent_id, name, path) VALUES (?, ?, '')";

	ASSERT(name != NULL);
	ASSERT(strlen(name) > 0);

	/* open db connection */
	if (mbox_library_local_open_database(&db, SQLITE_OPEN_READWRITE) == -1) {
		LOG_VPRINT_ERROR("Could not open database: %s",
			sqlite3_errmsg(db));
		goto end;
	}

	/* prepare the query */
	while ((res = sqlite3_prepare_v2(db, sql, -1, &stmt, 0)) != SQLITE_OK) {
		if (res == SQLITE_LOCKED) {
			usleep(100L * 1000L);
			continue;
		}
		errno = EFAULT;
		LOG_VPRINT_ERROR("Could not prepare SQL statement: %s", sql);
		LOG_VPRINT_ERROR("SQL Error: %s", sqlite3_errmsg(db));
		goto end;
	}

	/* bind parameters */
	if (sqlite3_bind_int(stmt, 1, parent_id) != SQLITE_OK ||
		sqlite3_bind_text(stmt, 2, name, strlen(name), NULL) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Binding failed: %s", sqlite3_errmsg(db));
		goto end;
	}

	/* execute the statement */
	while ((res = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (res == SQLITE_BUSY) {
			usleep(100L * 1000L);
			continue;
		} else if (res == SQLITE_ERROR) {
			LOG_VPRINT_ERROR("SQLite Error: %s", sqlite3_errmsg(db));
			goto end;
		} else if (res == SQLITE_MISUSE) {
			DEBUG_ABORT(LOG_MODULE, "Sqlite misuse!");
		}
	}

	/* get the row id */
	ret = sqlite3_last_insert_rowid(db);
end:
	if (stmt != NULL) {
		sqlite3_finalize(stmt);
	}
	if (db != NULL) {
		sqlite3_close(db);
	}
	return ret;
}


static char *
mbox_library_local_video_name(const char * const path, int64_t * const parent_id)
{
	int ret, i;
	char *name = NULL, *tmp = NULL, *res = NULL;
	const char *patterns[] =
	{
		"(.*)S([0-9][0-9])E([0-9][0-9])(.*)",
		"(.*)S([0-9][0-9])(.*)"
	};

	if ((tmp = strdup(path)) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}
	if ((name = strdup(basename(tmp))) == NULL) {
		goto end;
	}
	free(tmp);

	mbox_library_stripext(name);

	for (i = 0; i < (sizeof(patterns) / sizeof(char*)); i++) {
		regex_t regex;
		int nmatches = 5;
		regmatch_t matches[nmatches];

		if ((ret = regcomp(&regex, patterns[i], REG_EXTENDED | REG_ICASE)) != 0) {
			DEBUG_ABORT(LOG_MODULE, "Could not compile regexp!");
		}

		/* if the name looks like a TV serie... */
		if ((ret = regexec(&regex, name, nmatches, matches, 0)) == 0) {

			char buf[1024] = "", *pbuf;
			char *serie_name, *season, *episode;
			int64_t tmp_id;
			size_t matchlen;

			matchlen = matches[1].rm_eo - matches[1].rm_so;
			strncpy(buf, name + matches[1].rm_so, matchlen);
			buf[matchlen] = '\0';
			if ((serie_name = strdup(buf)) == NULL) {
				abort();
			}
			if ((serie_name = mbox_library_transform_video_title(serie_name)) == NULL) {
				abort();
			}
			if ((serie_name = strtrim(serie_name)) == NULL) {
				abort();
			}

			matchlen = matches[2].rm_eo - matches[2].rm_so;
			strcpy(buf, "Season ");
			strncat(buf, name + matches[2].rm_so, matchlen);
			buf[matchlen + 7] = '\0';
			if ((season = strdup(buf)) == NULL) {
				abort();
			}

			matchlen = matches[3].rm_eo - matches[3].rm_so;
			strcpy(buf, "Episode ");
			strncat(buf, name + matches[3].rm_so, matchlen);
			buf[matchlen + 8] = '\0';
			strcat(buf, " ");

			pbuf = &buf[matchlen + 9];
			matchlen = matches[4].rm_eo - matches[4].rm_so;
			strncpy(pbuf, name + matches[4].rm_so, matchlen);
			if ((episode = strdup(buf)) == NULL) {
				abort();
			}

			/* lookup or create serie directory */
			if ((*parent_id = mbox_library_local_getid(serie_name,
				MBOX_LIBRARY_LOCAL_DIRECTORY_SERIES)) == -1) {
				if ((*parent_id = mbox_library_local_mkdir(serie_name,
					MBOX_LIBRARY_LOCAL_DIRECTORY_SERIES)) == -1) {
					LOG_VPRINT_ERROR("Could not create series directory: %s",
						strerror(errno));
					res = NULL;
					goto serie_end;
				}
			}

			/* find or create the season directory */
			if ((tmp_id = mbox_library_local_getid(season, *parent_id)) == -1) {
				if ((*parent_id = mbox_library_local_mkdir(season, *parent_id)) == -1) {
					LOG_VPRINT_ERROR("Could not create season directory: %s",
						strerror(errno));
					res = NULL;
					goto serie_end;
				}
			} else {
				*parent_id = tmp_id;
			}

			/* copy and format the episode name */
			if ((res = strdup(episode)) == NULL) {
				LOG_PRINT_ERROR("Could not dup episode name");
			}
			if ((res = mbox_library_transform_video_title(res)) == NULL) {
				LOG_VPRINT_ERROR("Could not tranform video title: %s",
					strerror(errno));
			}
serie_end:
			free(episode);
			free(season);
			free(serie_name);

			goto end;

		} else if (ret == REG_NOMATCH) {
			/* nothing */
		} else {
			char buf[256];
			regerror(ret, &regex, buf, sizeof(buf));
			LOG_VPRINT_ERROR("Regex error: %s", buf);
		}

		regfree(&regex);
	}

	/* this is not a serie */
	*parent_id = MBOX_LIBRARY_LOCAL_DIRECTORY_MOVIES;
	name = mbox_library_transform_video_title(name);
	res = strdup(name);

end:
	if (name != NULL) {
		free(name);
	}

	return res;
};


static char *
mbox_library_local_audio_name(const char * const path, int64_t * const parent_id)
{
	*parent_id = 2;
	return strdup("Song");
}


/**
 * Add content to the local media library.
 */
static int64_t
mbox_library_addcontent(const char * const path)
{
	int ret = -1;
	magic_t magic;
	const char *mime = NULL;

	if ((magic = magic_open(MAGIC_MIME)) == NULL) {
		LOG_PRINT_ERROR("Could not create magic cookie");
		errno = EFAULT;
		return -1;
	}

	if (magic_load(magic, NULL) != 0) {
		LOG_VPRINT_ERROR("Could not load magic database: %s",
			magic_error(magic));
		magic_close(magic);
		errno = EFAULT;
		return -1;
	}

	if ((mime = magic_file(magic, path)) == NULL) {
		LOG_PRINT_ERROR("Could not get file magic");
		magic_close(magic);
		errno = EFAULT;
		return -1;
	}

	if ((!strncmp(mime, "video/", 6) && strcmp(mime, "video/subtitle")) || !strncmp(mime, "video/", 6)) {
		int res, retries = 0;
		int64_t id, parent_id;
		sqlite3 *db = NULL;
		sqlite3_stmt *stmt = NULL;
		char * name = NULL;
		const char * const sql_insert = "INSERT INTO local_objects (parent_id, name, path) VALUES (?, ?, ?)";
		const char * const sql_update = "UPDATE local_objects SET name = ? WHERE id = ?";

		if (!strcmp(path + (strlen(path) - 3), "sub")) {
			errno = EINVAL;
			goto end;
		}

		id = mbox_library_local_getid_by_uri(path);
		if (id == -1) {
			DEBUG_VPRINT(LOG_MODULE, "Adding '%s' to library", path);
		} else {
			DEBUG_VPRINT(LOG_MODULE, "Updating '%s' (%i)", path, id);
		}

		if (!strncmp(mime, "video/", 6)) {
			if ((name = mbox_library_local_video_name(path, &parent_id)) == NULL) {
				LOG_VPRINT_ERROR("Could not get video name: %s",
					strerror(errno));
				goto end;
			}
		} else {
			ASSERT(!strncmp(mime, "audio/", 6));
			if ((name = mbox_library_local_audio_name(path, &parent_id)) == NULL) {
				goto end;
			}
		}
		if (!strcmp(name, "")) {
			errno = EINVAL;
			goto end;
		}

		/* add the file to the mediatomb database */
		#if 0
		if (mbox_library_mediatombadd(path) != 0) {
			LOG_VPRINT_ERROR("mediatomb --add '%s' failed!",
				path);
		}
		#endif

		/* open db connection */
		if (mbox_library_local_open_database(&db, SQLITE_OPEN_READWRITE) == -1) {
			LOG_VPRINT_ERROR("Could not open database: %s",
				sqlite3_errmsg(db));
			goto end;
		}

		if (id == -1) {
			/* prepare the query */
			while ((res = sqlite3_prepare_v2(db, sql_insert, -1, &stmt, 0)) != SQLITE_OK) {
				if (res == SQLITE_LOCKED) {
					if (retries < 3) {
						usleep(100L * 1000L);
						retries++;
						continue;
					}
					errno = EAGAIN;
				} else {
					errno = EFAULT;
				}
				LOG_VPRINT_ERROR("Could not prepare SQL statement: %s", sql_insert);
				LOG_VPRINT_ERROR("SQL Error: %s", sqlite3_errmsg(db));
				goto end;
			}

			/* bind parameters */
			if (sqlite3_bind_int(stmt, 1, parent_id) != SQLITE_OK ||
				sqlite3_bind_text(stmt, 2, name, strlen(name), NULL) != SQLITE_OK ||
				sqlite3_bind_text(stmt, 3, path, strlen(path), NULL) != SQLITE_OK) {
				LOG_VPRINT_ERROR("Binding failed: %s", sqlite3_errmsg(db));
				goto end;
			}
		} else {
			/* prepare the query */
			while ((res = sqlite3_prepare_v2(db, sql_update, -1, &stmt, 0)) != SQLITE_OK) {
				if (res == SQLITE_LOCKED) {
					if (retries < 3) {
						usleep(100L * 1000L);
						retries++;
						continue;
					}
					errno = EAGAIN;
				} else {
					errno = EFAULT;
				}
				LOG_VPRINT_ERROR("Could not prepare SQL statement: %s", sql_insert);
				LOG_VPRINT_ERROR("SQL Error: %s", sqlite3_errmsg(db));
				goto end;
			}

			/* bind parameters */
			if (sqlite3_bind_text(stmt, 1, name, strlen(name), NULL) != SQLITE_OK ||
				sqlite3_bind_int(stmt, 2, id) != SQLITE_OK) {
				LOG_VPRINT_ERROR("Binding failed: %s", sqlite3_errmsg(db));
				goto end;
			}
		}

		/* execute the statement */
		while ((res = sqlite3_step(stmt)) != SQLITE_DONE) {
			if (res == SQLITE_BUSY) {
				usleep(100L * 1000L);
				continue;
			} else if (res == SQLITE_ERROR) {
				LOG_VPRINT_ERROR("SQLite Error: %s", sqlite3_errmsg(db));
				break;
			} else if (res == SQLITE_MISUSE) {
				DEBUG_ABORT(LOG_MODULE, "Sqlite misuse!");
			}
		}

		ret = sqlite3_last_insert_rowid(db);
end:
		if (name != NULL) {
			free(name);
		}
		if (stmt != NULL) {
			sqlite3_finalize(stmt);
		}
		if (db != NULL) {
			sqlite3_close(db);
		}
	} else {
		errno = EINVAL;
	}

	magic_close(magic);

	return ret;
}


/**
 * Add a directory to the watch list.
 */
int
mbox_library_scandir(const char * const path)
{
	DIR *dir;
	struct dirent *ent;
	int ret = -1;

	DEBUG_VPRINT(LOG_MODULE, "Scanning '%s'...", path);

	if ((dir = opendir(path)) == NULL) {
		LOG_VPRINT_ERROR("Could not watch path (%s): %s",
			path, strerror(errno));
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		char *entpath;
		struct stat st;

		if (ent->d_name[0] == '.') {
			continue;
		}

		entpath = malloc(strlen(path) + 1 + strlen(ent->d_name) + 1);
		if (entpath == NULL) {
			ASSERT(errno == ENOMEM);
			DEBUG_PRINT(LOG_MODULE, "Library scan aborted. Out of memory");
			goto end;
		}

		strcpy(entpath, path);
		strcat(entpath, "/");
		strcat(entpath, ent->d_name);

		if (stat(entpath, &st) == -1) {
			LOG_VPRINT_ERROR("Could not stat %s: %s",
				entpath, strerror(errno));
			free(entpath);
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			if (mbox_library_scandir(entpath) == -1) {
				LOG_VPRINT_ERROR("Could not scan directory '%s': %s",
					entpath, strerror);
			}
		} else {
			if (mbox_library_addcontent(entpath) == -1) {
				if (errno != EINVAL) {
					LOG_VPRINT_ERROR("Could not add content '%s': %s",
						entpath, strerror(errno));
				}
			}
		}

		free(entpath);
	}

	ret = 0;
end:
	if (dir != NULL) {
		closedir(dir);
	}

	return ret;
}


static void *
mbox_library_local_scan_library(void * const arg)
{
	DEBUG_PRINT(LOG_MODULE, "Scanning media library...");
	mbox_library_scandir(MBOX_STORE_VIDEO);
	mbox_library_scandir(MBOX_STORE_AUDIO);
	DEBUG_PRINT(LOG_MODULE, "Library scan complete.");
	return NULL;
}


/**
 * Check if the local database exists and create it if it
 * doesn't.
 */
static int
mbox_library_create_db_if_not_exist()
{
	int ret = -1;
	char *filename;
	struct stat st;
	struct avbox_delegate *del;

	if ((filename = avbox_dbutil_getdbfile("content.db")) == NULL) {
		ASSERT(errno == ENOMEM);
		return ret;
	}

	while (stat(filename, &st) == -1) {

		int res;
		sqlite3 *db = NULL;

		if (errno == EAGAIN || errno == EINTR) {
			continue;
		} else if (errno != ENOENT) {
			LOG_VPRINT_ERROR("Cannot stat content database: %s",
				strerror(errno));
			goto end;
		}

		DEBUG_VPRINT(LOG_MODULE, "Could not find database %s. Creating",
			filename);

		const char * const sql =
			"CREATE TABLE local_objects ("
			"	id INTEGER PRIMARY KEY,"
			"	parent_id INTEGER,"
			"	name TEXT,"
			"	path TEXT,"
			"	date_added INTEGER,"
			"	date_modified INTEGER"
			");"
			"INSERT INTO local_objects (id, parent_id, name, path) VALUES (1, 0, 'Audio', '');"
			"INSERT INTO local_objects (id, parent_id, name, path) VALUES (2, 0, 'Video', '');"
			"INSERT INTO local_objects (id, parent_id, name, path) VALUES (3, 2, 'Movies', '');"
			"INSERT INTO local_objects (id, parent_id, name, path) VALUES (4, 2, 'TV Shows', '');";

		/* open database */
		if (mbox_library_local_open_database(&db,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) == -1) {
			ASSERT(errno == EIO);
			LOG_VPRINT_ERROR("Could not open database: %s",
				sqlite3_errmsg(db));
			goto end;
		}

		if ((res = sqlite3_exec(db, sql, NULL, NULL, NULL)) != SQLITE_OK) {
			LOG_VPRINT_ERROR("SQL Query: '%s' failed (%d)!", sql, res);
			LOG_VPRINT_ERROR("SQL Error: %s", sqlite3_errmsg(db));
			sqlite3_close(db);
			goto end;
		}

		sqlite3_close(db);

		/* scan the internal storage in background thread */
		if ((del = avbox_workqueue_delegate(
			mbox_library_local_scan_library, NULL)) == NULL) {
			LOG_VPRINT_ERROR("Could not start scan worker: %s",
				strerror(errno));
		} else {
			avbox_delegate_dettach(del);
		}

		break;
	}

	ret = 0;
end:
	if (filename != NULL) {
		free(filename);
	}

	return ret;
}


static struct mbox_library_dir *
mbox_library_local_opendir(const char * const path)
{
	int64_t id;
	int res;
	struct mbox_library_dir *ret = NULL;
	const char * const ppath = path + 6;
	const char * const sql = "SELECT id, name, path FROM local_objects WHERE parent_id = ? ORDER BY name;";
	struct mbox_library_dir *dir = NULL;


	/* get the id of the directory */
	if ((id = mbox_library_local_getid(ppath, 0)) == -1) {
		DEBUG_VPRINT(LOG_MODULE, "Could not get id for %s",
			ppath);
		errno = ENOENT;
		return NULL;
	}

	if ((dir = malloc(sizeof(struct mbox_library_dir))) == NULL) {
		errno = ENOMEM;
		return NULL;
	}


	/* open db connection */
	if (mbox_library_local_open_database(
		&dir->state.localdir.db, SQLITE_OPEN_READONLY) == -1) {
		LOG_VPRINT_ERROR("Could not open database: %s",
			sqlite3_errmsg(dir->state.localdir.db));
		errno = EIO;
		goto end;
	}

	/* prepare the query */
	while ((res = sqlite3_prepare_v2(dir->state.localdir.db, sql,
		-1, &dir->state.localdir.stmt, 0)) != SQLITE_OK) {
		if (res == SQLITE_LOCKED) {
			usleep(100L * 1000L);
			continue;
		} else {
			errno = EFAULT;
			LOG_VPRINT_ERROR("Could not prepare SQL statement: %s", sql);
			LOG_VPRINT_ERROR("SQL Error: %s", sqlite3_errmsg(dir->state.localdir.db));
			goto end;
		}
	}

	/* bind parameter */
	if (sqlite3_bind_int(dir->state.localdir.stmt, 1, id) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Could not bind parameter: %s", sql);
		LOG_VPRINT_ERROR("SQL Error: %s", sqlite3_errmsg(dir->state.localdir.db));
		goto end;

	}

	dir->type = MBOX_LIBRARY_DIRTYPE_LOCAL;
	dir->state.localdir.dotdot_sent = 0;
	ret = dir;
end:
	if (ret == NULL) {
		if (dir->state.localdir.stmt != NULL) {
			sqlite3_finalize(dir->state.localdir.stmt);
			dir->state.localdir.stmt = NULL;
		}
		if (dir->state.localdir.db != NULL) {
			sqlite3_close(dir->state.localdir.db);
			dir->state.localdir.db = NULL;
		}
		if (dir != NULL) {
			free(dir);
		}
	}
	return ret;
}


static struct mbox_library_dirent *
mbox_library_local_readdir(struct mbox_library_dir * const dir)
{
	int ret;
	struct mbox_library_dirent *ent = NULL;

	ASSERT(dir->state.localdir.db != NULL);
	ASSERT(dir->state.localdir.stmt != NULL);

	if ((ent = malloc(sizeof(struct mbox_library_dirent))) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}

	if (!dir->state.localdir.dotdot_sent) {
		if ((ent->name = strdup("..")) == NULL) {
			ASSERT(errno == ENOMEM);
			free(ent);
			return NULL;
		}
		if ((ent->path = mbox_library_striplastlevel(dir->path)) == NULL) {
			ASSERT(errno == ENOMEM);
			free(ent->name);
			free(ent);
			return NULL;
		}
		ent->isdir = 1;
		dir->state.localdir.dotdot_sent = 1;
		return ent;
	}

	/* fetch the next row */
	if ((ret = sqlite3_step(dir->state.localdir.stmt)) != SQLITE_ROW) {
		if (ret == SQLITE_BUSY) {
			errno = EAGAIN;
		} else {
			errno = ENOMEM;
		}
		free(ent);
		return NULL;
	}

	ent->isdir = (sqlite3_column_text(dir->state.localdir.stmt, 2)[0] == '\0'); /* path */
	ent->name = strdup((const char *) sqlite3_column_text(dir->state.localdir.stmt, 1));
	if (ent->name == NULL) {
		ASSERT(errno == ENOMEM);
		free(ent);
		return NULL;
	}

	if (ent->isdir) {
		ent->path = malloc(strlen(dir->path) + 1 + strlen(ent->name) + 1);
		if (ent->path == NULL) {
			free(ent->name);
			free(ent);
			return NULL;
		}
		strcpy(ent->path, dir->path);
		if (ent->path[strlen(ent->path) - 1] != '/') {
			strcat(ent->path, "/");
		}
		strcat(ent->path, ent->name);
	} else {
		ent->path = strdup((const char *) sqlite3_column_text(dir->state.localdir.stmt, 2));
		if (ent->path == NULL) {
			free(ent->name);
			free(ent);
			return NULL;
		}
	}

	return ent;
}


static struct mbox_library_dirent *
mbox_library_dotdot(struct mbox_library_dir * const dir)
{
	struct mbox_library_dirent *ent;

	/* create the directory entry */
	if ((ent = malloc(sizeof(struct mbox_library_dirent))) == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}
	ent->isdir = 0;
	if ((ent->name = strdup("..")) == NULL) {
		ASSERT(errno == ENOMEM);
		free(ent);
		return NULL;
	}
	if ((ent->path = mbox_library_striplastlevel(dir->path)) == NULL) {
		ASSERT(errno == ENOMEM);
		free(ent->name);
		free(ent);
		return NULL;
	}
	return ent;
}


/**
 * Open a library directory
 */
struct mbox_library_dir *
mbox_library_opendir(const char * const path)
{
	struct mbox_library_dir *dir;

	if (!strcmp("/", path)) {

		if ((dir = malloc(sizeof(struct mbox_library_dir))) == NULL) {
			errno = ENOMEM;
			return NULL;
		}

		dir->type = MBOX_LIBRARY_DIRTYPE_ROOT;
		dir->state.rootdir.ptr = NULL;

		LIST_INIT(&dir->state.rootdir.entries);

		mbox_library_adddirent("Local Files", "/local", 1, &dir->state.rootdir.entries);
		mbox_library_adddirent("UPnP Devices", "/upnp", 1, &dir->state.rootdir.entries);
		mbox_library_adddirent("TV Tunners", "/tv", 1, &dir->state.rootdir.entries);
#if defined(ENABLE_DVD)
		mbox_library_adddirent("Optical Discs", "/dvd", 1, &dir->state.rootdir.entries);
#endif
#ifdef ENABLE_BLUETOOTH
		if (avbox_bluetooth_ready()) {
			mbox_library_adddirent("Bluetooth Devices", "/bluetooth", 1, &dir->state.rootdir.entries);
		}
#endif
	} else if (!strncmp("/local", path, 6)) {
		if ((dir = mbox_library_local_opendir(path)) == NULL) {
			return NULL;
		}

	} else if (!strncmp("/upnp", path, 5)) {
		char *rpath;
		char resolved_path[PATH_MAX];
		const char * const ppath = (path + 5);
		const int path_len = strlen(path + 5);

		if ((dir = malloc(sizeof(struct mbox_library_dir))) == NULL) {
			errno = ENOMEM;
			return NULL;
		}

		if ((rpath = malloc(sizeof(UPNP_ROOT) + path_len + 2)) == NULL) {
			free(dir->path);
			free(dir);
			errno = ENOMEM;
			return NULL;
		}

		strcpy(rpath, UPNP_ROOT);
		strcat(rpath, ppath);

		if (ppath[path_len - 1] != '/') {
			strcat(rpath, "/");
		}

		if (realpath(rpath, resolved_path) == NULL) {
			DEBUG_VPRINT(LOG_MODULE, "Could not resolve path: %s",
				rpath);
			free(rpath);
			free(dir->path);
			free(dir);
			return NULL;
		}

		if ((dir->state.upnpdir.path = strdup(resolved_path)) == NULL) {
			free(rpath);
			free(dir->path);
			free(dir);
		}

		dir->type = MBOX_LIBRARY_DIRTYPE_UPNP;
		dir->state.upnpdir.dir = opendir(rpath);
		if (dir->state.upnpdir.dir == NULL) {
			DEBUG_VPRINT(LOG_MODULE, "opendir(\"%s\") failed: %s",
				resolved_path, strerror(errno));
			free(rpath);
			free(dir->path);
			free(dir);
		}

		free(rpath);

#if defined(ENABLE_DVD)
	} else if (!strncmp("/dvd", path, 4)) {

		struct udev_enumerate *udev_enum;
		if ((dir = malloc(sizeof(struct mbox_library_dir))) == NULL) {
			errno = ENOMEM;
			return NULL;
		}

		dir->type = MBOX_LIBRARY_DIRTYPE_DVD;
		dir->state.discdir.read = 0;
		if ((udev_enum = udev_enumerate_new(udev)) == NULL) {
			errno = EFAULT;
			free(dir);
			return NULL;
		}
		if (udev_enumerate_scan_devices(udev_enum) == -1) {
			errno = EFAULT;
			LOG_PRINT_ERROR("Could not enumerate devices!");
			free(dir);
			return NULL;
		}
		dir->state.discdir.udev_enum = udev_enum;
		dir->state.discdir.list =
			udev_enumerate_get_list_entry(udev_enum);
#endif
#ifdef ENABLE_BLUETOOTH
	} else if (!strncmp("/bluetooth", path, 10)) {

		if ((dir = malloc(sizeof(struct mbox_library_dir))) == NULL) {
			errno = ENOMEM;
			return NULL;
		}

		dir->type = MBOX_LIBRARY_DIRTYPE_BLUETOOTH;
		dir->state.btdir.read = 0;
		dir->state.btdir.devs = avbox_bluetooth_getdevices(AVBOX_BT_A2DP_UUID);
		dir->state.btdir.cur = dir->state.btdir.devs;
		if (dir->state.btdir.devs == NULL) {
			errno = ENOTSUP;
			free(dir);
			return NULL;
		}
#endif
	} else if (!strncmp("/tv", path, 3)) {

		if ((dir = malloc(sizeof(struct mbox_library_dir))) == NULL) {
			errno = ENOMEM;
			return NULL;
		}

		dir->type = MBOX_LIBRARY_DIRTYPE_TV;
		dir->state.emptydir.read = 0;

	} else {
		DEBUG_PRINT(LOG_MODULE, "Invalid path!");
		abort();
	}

	if (dir != NULL && (dir->path = strdup(path)) == NULL) {
		errno = ENOMEM;
		/* TODO: WE should closedir() in here */
		return NULL;
	}

	return dir;
}


/**
 * Read the next entry in an open directory.
 */
struct mbox_library_dirent *
mbox_library_readdir(struct mbox_library_dir * const dir)
{
	switch (dir->type) {
	case MBOX_LIBRARY_DIRTYPE_ROOT:
	{
		if (dir->state.rootdir.ptr == NULL) {
			dir->state.rootdir.ptr = LIST_NEXT(struct mbox_library_dirent*,
				&dir->state.rootdir.entries);
		} else {
			dir->state.rootdir.ptr = LIST_NEXT(struct mbox_library_dirent*,
				dir->state.rootdir.ptr);
		}
		if (dir->state.rootdir.ptr == NULL ||
			LIST_ISNULL(&dir->state.rootdir.entries, dir->state.rootdir.ptr)) {
			errno = ENOENT;
			return NULL;
		}
		return mbox_library_dupdirent(dir->state.rootdir.ptr);
	}
	case MBOX_LIBRARY_DIRTYPE_LOCAL:
	{
		return mbox_library_local_readdir(dir);
	}
	case MBOX_LIBRARY_DIRTYPE_UPNP:
	{
		struct dirent *dirent;

		while ((dirent = readdir(dir->state.upnpdir.dir)) != NULL) {

			struct mbox_library_dirent *ent;
			char *filename, *ext = NULL, *filepath, *filepathrel;
			struct stat st;

			/* do not show dot directories except .. */
			if (dirent->d_name[0] == '.') {
				if (strcmp(dirent->d_name, "..")) {
					continue;
				}
			}

			/* do not show the dot directory */
			if (dirent->d_name[0] == '.' && dirent->d_name[1] == '\0') {
				continue;
			}

			/* get the file extension */
			if ((filename = strdup(dirent->d_name)) != NULL) {
				ext = mbox_library_stripext(filename);
			}

			/* do not show subtitles */
			if (ext != NULL) {
				if (!strcasecmp("srt", ext) ||
					!strcasecmp("sub", ext) ||
					!strcasecmp("idx", ext)) {
					free(filename);
					continue;
				}
			} else {
				free(filename);
				filename = NULL;
			}

			/* allocate mem for a copy of the filepath */
			filepath = malloc(strlen(dir->state.upnpdir.path) + strlen(dirent->d_name) + 3);
			if (filepath == NULL) {
				LOG_VPRINT_ERROR("Could not load list: %s",
					strerror(errno));
				return NULL;
			}

			strcpy(filepath, dir->state.upnpdir.path);
			strcat(filepath, "/");
			strcat(filepath, dirent->d_name);

			if (stat(filepath, &st) == -1) {
				LOG_VPRINT_ERROR("Could not stat '%s': %s",
					filepath, strerror(errno));
				free(filepath);
				continue;
			}
			if (S_ISDIR(st.st_mode)) {
				strcat(filepath, "/");
				filepathrel = strdup(filepath + sizeof(UPNP_ROOT) - 1);
			} else {
				filepathrel = strdup(filepath);
			}
			if (filepathrel == NULL) {
				ASSERT(errno == ENOMEM);
				LOG_VPRINT_ERROR("Could not load list: %s",
					strerror(errno));
				free(filepath);
				return NULL;
			}

			free(filepath);

			/* create the entry */
			if ((ent = malloc(sizeof(struct mbox_library_dirent))) == NULL) {
				free(filepathrel);
				return NULL;
			}

			if ((ent->name = strdup(dirent->d_name)) == NULL) {
				ASSERT(errno == ENOMEM);
				free(ent);
				return NULL;
			}

			if (S_ISDIR(st.st_mode)) {
				if (!strcmp(dirent->d_name, "..")) {
					if ((ent->path =
						mbox_library_striplastlevel(dir->path)) == NULL) {
						ASSERT(errno == ENOMEM);
						free(filepathrel);
						return NULL;
					}

				} else {
					if ((ent->path = malloc(5 + strlen(filepathrel) + 1)) == NULL) {
						ASSERT(errno == ENOMEM);
						free(filepathrel);
						return NULL;
					}
					strcpy(ent->path, "/upnp");
					strcat(ent->path, filepathrel);
				}
				ent->isdir = 1;
				free(filepathrel);
			} else {
				ent->path = filepathrel;
				ent->isdir = 0;
			}

			return ent;
		}

		return NULL;
	}
#ifdef ENABLE_BLUETOOTH
	case MBOX_LIBRARY_DIRTYPE_BLUETOOTH:
	{
		struct mbox_library_dirent *ent;
		struct avbox_btdev *dev = *dir->state.btdir.cur;

		ASSERT(dir->state.btdir.devs != NULL);
		ASSERT(dir->state.btdir.cur != NULL);

		if (!dir->state.btdir.read) {
			dir->state.btdir.read = 1;
			return mbox_library_dotdot(dir);
		}

		/* skip until the first connected device */
		while (*dir->state.btdir.cur != NULL) {
			if ((*dir->state.btdir.cur)->connected) {
				break;
			}
			dir->state.btdir.cur++;
		}

		if ((dev = *dir->state.btdir.cur) == NULL) {
			return NULL;
		}

		const int name_len = strlen(dev->name) + 2 + strlen(dev->address) + 1;
		const int path_len = 5 + strlen(dev->address);

		if ((ent = malloc(sizeof(struct mbox_library_dirent))) == NULL) {
			ASSERT(errno == ENOMEM);
			return NULL;
		}

		if ((ent->name = malloc(name_len + 1)) == NULL) {
			ASSERT(errno == ENOMEM);
			free(ent);
			return NULL;
		}

		if ((ent->path = malloc(path_len + 1)) == NULL) {
			ASSERT(errno == ENOMEM);
			free(ent->name);
			free(ent);
			return NULL;
		}

		ent->isdir = 0;
		snprintf(ent->name, name_len + 1, "%s (%s)",
			dev->name, dev->address);
		snprintf(ent->path, path_len + 1, "a2dp:%s",
			dev->address);

		/* move to next entry */
		dir->state.btdir.cur++;

		return ent;
	}
#endif
#if defined(ENABLE_DVD)
	case MBOX_LIBRARY_DIRTYPE_DVD:
	{
		struct mbox_library_dirent *ent;
		struct udev_list_entry *udev_ent;

		if (!dir->state.discdir.read) {
			dir->state.discdir.read = 1;
			return mbox_library_dotdot(dir);
		} else if (dir->state.discdir.read == 1) {
			dir->state.discdir.read = 2;
			if ((ent = malloc(sizeof(struct mbox_library_dirent))) == NULL) {
				ASSERT(errno == ENOMEM);
				return NULL;
			}
			if ((ent->name = strdup("Play DVD")) == NULL) {
				free(ent);
				return NULL;
			}
			if ((ent->path = strdup("dvd:/dev/sr0")) == NULL) {
				free(ent->name);
				free(ent);
				return  NULL;
			}
			return ent;
		} else if (dir->state.discdir.read == 2) {
			return NULL;
		}

		while (dir->state.discdir.list != NULL) {
			struct udev_device *dev;

			/* move iterator to next item */
			udev_ent = dir->state.discdir.list;
			dir->state.discdir.list = udev_list_entry_get_next(
				dir->state.discdir.list);

			if ((dev = udev_device_new_from_syspath(udev,
				udev_list_entry_get_name(udev_ent))) == NULL) {
				LOG_VPRINT_ERROR("Could not get device structure for: %s",
					udev_list_entry_get_name(udev_ent));
				continue;
			}

			if (0 && udev_device_get_devnum(dev) != 11) {
				udev_device_unref(dev);
				continue;
			}
			if (0 & strcmp(udev_device_get_subsystem(dev), "block")) {
				udev_device_unref(dev);
				continue;
			}

			const char * type = udev_device_get_devtype(dev);
			const char * node = udev_device_get_devnode(dev);
			const char * subs = udev_device_get_subsystem(dev);
			const char * path = "dvd1";

			if (type == NULL) {
				type = "unknown";
			}
			if (node == NULL) {
				node = udev_device_get_devpath(dev);
				if (node == NULL) {
					node = "";
				}
			}

			if ((ent = malloc(sizeof(struct mbox_library_dirent))) == NULL) {
				ASSERT(errno == ENOMEM);
				udev_device_unref(dev);
				return NULL;
			}

			if ((ent->name = malloc(6+ strlen(subs) + 1 + strlen(type) + 2 + strlen(node) + 2)) == NULL) {
				ASSERT(errno == ENOMEM);
				free(ent);
				return NULL;
			}
			sprintf(ent->name, "%li %s %s (%s)",
				udev_device_get_devnum(dev), subs, type, node);
			if ((ent->path = strdup(path)) == NULL) {
				ASSERT(errno == ENOMEM);
				free(ent->name);
				free(ent);
				return NULL;
			}
			ent->isdir = 0;
			udev_device_unref(dev);
			return ent;
		}
		return NULL;
	}
#endif
	case MBOX_LIBRARY_DIRTYPE_TV:
	{
		/* if we already read this directory then return EOF */
		if (!dir->state.emptydir.read) {
			dir->state.emptydir.read = 1;
			return mbox_library_dotdot(dir);
		}
		return NULL;
	}
	default:
		abort();
	}
}


/**
 * Free the directory entry.
 */
void
mbox_library_freedirentry(struct mbox_library_dirent * const ent)
{
	if (ent->name != NULL) {
		free(ent->name);
	}
	if (ent->path != NULL) {
		free(ent->path);
	}
	free(ent);
}


/**
 * Close a library directory.
 */
void
mbox_library_closedir(struct mbox_library_dir * const dir)
{
	ASSERT(dir != NULL);
	ASSERT(dir->path != NULL);

	switch (dir->type) {
	case MBOX_LIBRARY_DIRTYPE_ROOT:
	{
		struct mbox_library_dirent *ent;
		LIST_FOREACH_SAFE(struct mbox_library_dirent*, ent, &dir->state.rootdir.entries, {
			LIST_REMOVE(ent);
			mbox_library_freedirentry(ent);
		});
		break;
	}
	case MBOX_LIBRARY_DIRTYPE_LOCAL:
	{
		ASSERT(dir->state.localdir.db != NULL);
		ASSERT(dir->state.localdir.stmt != NULL);
		sqlite3_finalize(dir->state.localdir.stmt);
		sqlite3_close(dir->state.localdir.db);
		dir->state.localdir.stmt = NULL;
		dir->state.localdir.db = NULL;
		break;
	}
	case MBOX_LIBRARY_DIRTYPE_UPNP:
	{
		ASSERT(dir->state.upnpdir.path != NULL);
		free(dir->state.upnpdir.path);
		closedir(dir->state.upnpdir.dir);
		break;
	}
#ifdef ENABLE_BLUETOOTH
	case MBOX_LIBRARY_DIRTYPE_BLUETOOTH:
	{
		ASSERT(dir->state.btdir.devs != NULL);
		dir->state.btdir.cur = dir->state.btdir.devs;
		while (*dir->state.btdir.cur != NULL) {
			avbox_bluetooth_freedev(*dir->state.btdir.cur);
			dir->state.btdir.cur++;
		}
		free(dir->state.btdir.devs);
		break;
	}
#endif
#if defined(ENABLE_DVD)
	case MBOX_LIBRARY_DIRTYPE_DVD:
	{
		udev_enumerate_unref(dir->state.discdir.udev_enum);
		break;
	}
#endif
	case MBOX_LIBRARY_DIRTYPE_TV:
	{
		break;
	}
	default:
		abort();
	}

	free(dir->path);
	free(dir);
}


static int
mbox_library_local_add_watch(const char * const path)
{
	DIR *dir;
	struct dirent *ent;
	struct mbox_library_local_watchdir *watch_dir;

	if ((watch_dir = malloc(sizeof(struct mbox_library_local_watchdir))) == NULL) {
		ASSERT(errno == ENOMEM);
		return -1;
	}

	if ((watch_dir->path = strdup(path)) == NULL) {
		ASSERT(errno == ENOMEM);
		free(watch_dir);
		return -1;
	}

	/* create the inotify watch */
	if ((watch_dir->watch_fd = inotify_add_watch(
		local_inotify_fd, watch_dir->path,
		IN_CREATE | IN_DELETE | IN_DELETE_SELF |
		IN_CLOSE_WRITE | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO)) == -1) {
		LOG_VPRINT_ERROR("Could not add watch: %s",
			strerror(errno));
		free(watch_dir->path);
		free(watch_dir);
		return -1;
	}

	if ((dir = opendir(watch_dir->path)) == NULL) {
		LOG_VPRINT_ERROR("Could not open directory: '%s'", path);
		inotify_rm_watch(local_inotify_fd, watch_dir->watch_fd);
		free(watch_dir->path);
		free(watch_dir);
		return -1;
	}

	while (1) {
		struct stat st;
		char * child_path = NULL;

		if (!(errno = 0) && (ent = readdir(dir)) == NULL) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			} else if (errno == 0) {
				break;
			} else {
				LOG_VPRINT_ERROR("Could not read watch dir: %s",
					strerror(errno));
				inotify_rm_watch(local_inotify_fd, watch_dir->watch_fd);
				free(watch_dir->path);
				free(watch_dir);
				return -1;
			}
		}

		if (ent->d_name[0] == '.' || !strcmp(ent->d_name, "lost+found")) {
			continue;
		}

		/* build the path */
		if ((child_path = malloc(strlen(path) + 1 + strlen(ent->d_name) + 1)) == NULL) {
			ASSERT(errno == ENOMEM);
			break;
		}
		strcpy(child_path, watch_dir->path);
		if (path[strlen(watch_dir->path) - 1] != '/') {
			strcat(child_path, "/");
		}
		strcat(child_path, ent->d_name);

		if (stat(child_path, &st) != -1) {
			if (S_ISDIR(st.st_mode)) {
				(void) mbox_library_local_add_watch(child_path);
			}
		}
		free(child_path);
	}

	LIST_ADD(&local_inotify_watches, watch_dir);

	return 0;
}


static void *
mbox_library_local_inotify(void * const arg)
{
	char buf[1024];
	struct inotify_event *event = (struct inotify_event*) buf;

	DEBUG_SET_THREAD_NAME("library-inotify");
	DEBUG_PRINT(LOG_MODULE, "Starting inotify loop");

	while (!local_inotify_quit) {

		if (read(local_inotify_fd, buf, sizeof(buf)) == -1) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			} else {
				LOG_VPRINT_ERROR("Inotify read failed: %s",
					strerror(errno));
			}
		}

		if (event->len > 0) {

			char *dir_path = NULL, *path;
			struct mbox_library_local_watchdir *watchdir;
			LIST_FOREACH(struct mbox_library_local_watchdir*,
				watchdir, &local_inotify_watches) {
				if (event->wd == watchdir->watch_fd) {
					dir_path = watchdir->path;
				}
			}

			if (dir_path == NULL) {
				DEBUG_VPRINT(LOG_MODULE, "Event for unkown descriptor %i",
					event->wd);
				continue;
			}

			/* build the full path
			 * Note that we are allocating an extra byte in case
			 * we need to append a % for the sql statement */
			if ((path = malloc(strlen(dir_path) + 1 + strlen(event->name) + 2)) == NULL) {
				abort();
			}
			strcpy(path, dir_path);
			if (dir_path[strlen(dir_path) - 1] != '/') {
				strcat(path, "/");
			}
			strcat(path, event->name);

			if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
				/* these are illegal. for now we just abort(). in the future
				 * we should display a message on the ui and then abort() */
				abort();
			} else if (event->mask & IN_MOVED_TO) {
				struct stat st;

				DEBUG_VPRINT(LOG_MODULE, "File/directory moved in: %s",
					path);

				if (stat(path, &st) == -1) {
					LOG_VPRINT_ERROR("Could not stat '%s': %s",
						path, strerror(errno));
				} else {
					if (S_ISDIR(st.st_mode)) {
						/* scan the directory and add it to the
						 * watch list */
						mbox_library_local_add_watch(path);
						mbox_library_scandir(path);
					} else {
						if (mbox_library_addcontent(path) == -1) {
							if (errno != EINVAL) {
								LOG_VPRINT_ERROR("Could not create add '%s': %s",
									path, strerror(errno));
							}
						}
					}
				}

			} else if (event->mask & IN_CREATE) {
				struct stat st;

				DEBUG_VPRINT(LOG_MODULE, "File/directory created: %s",
					path);

				if (stat(path, &st) == -1) {
					LOG_VPRINT_ERROR("Could not stat '%s': %s",
						path, strerror(errno));
				} else {
					if (S_ISDIR(st.st_mode)) {
						mbox_library_local_add_watch(path);

						/* in a perfect world we don't need to scan
						 * here but since the message may be delayed
						 * (or read late) there may be new files in before
						 * we can add the watch */
						mbox_library_scandir(path);
					} else {
						/* we just wait for the IN_CLOSE_WRITE event */
					}
				}


			} else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
				int res;
				sqlite3 *db = NULL;
				sqlite3_stmt *stmt = NULL;
				const char * const sql = "DELETE FROM local_objects WHERE path LIKE ?";

				DEBUG_VPRINT(LOG_MODULE, "File deleted/moved out: %s",
					path);

				/* append wildcard to path */
				strcat(path, "%");

				/* open db connection */
				if (mbox_library_local_open_database(&db, SQLITE_OPEN_READWRITE) == -1) {
					LOG_VPRINT_ERROR("Could not open database: %s",
						sqlite3_errmsg(db));
					errno = EIO;
					goto remove_end;
				}


				/* prepare the query */
				while ((res = sqlite3_prepare_v2(db, sql, -1, &stmt, 0)) != SQLITE_OK) {
					if (res == SQLITE_LOCKED) {
						usleep(100L * 1000L);
						continue;
					} else {
						errno = EFAULT;
					}
					LOG_VPRINT_ERROR("Could not prepare SQL statement: %s", sql);
					LOG_VPRINT_ERROR("SQL Error: %s", sqlite3_errmsg(db));
					goto remove_end;
				}

				/* bind parameters */
				if (sqlite3_bind_text(stmt, 1, path, strlen(path), NULL) != SQLITE_OK) {
					LOG_VPRINT_ERROR("Binding failed: %s", sqlite3_errmsg(db));
					goto remove_end;
				}

				/* execute the statement */
				while ((res = sqlite3_step(stmt)) != SQLITE_DONE) {
					if (res == SQLITE_BUSY) {
						usleep(100L * 1000L);
						continue;
					} else if (res == SQLITE_ERROR) {
						LOG_VPRINT_ERROR("SQLite Error: %s", sqlite3_errmsg(db));
						break;
					} else if (res == SQLITE_MISUSE) {
						DEBUG_ABORT(LOG_MODULE, "Sqlite misuse!");
					}
				}
remove_end:
				if (stmt != NULL) {
					sqlite3_finalize(stmt);
				}
				if (db != NULL) {
					sqlite3_close(db);
				}

			} else if (event->mask & (IN_CLOSE_WRITE)) {
				DEBUG_VPRINT(LOG_MODULE, "File closed: %s",
					path);
				if (mbox_library_addcontent(path) == -1) {
					if (errno != EINVAL) {
						LOG_VPRINT_ERROR("Could not add '%s': %s",
							path, strerror(errno));
					}
				}
			} else if (event->mask & (IN_MODIFY)) {
				DEBUG_VPRINT(LOG_MODULE, "File modified: %s",
					path);
			}

			free(path);
		}
	}

	DEBUG_PRINT(LOG_MODULE, "inotify thread exitting");

	return NULL;
}


static int
mbox_library_local_init()
{
	struct stat st;

	/* if a store was specified then mount it */
	if (store != NULL) {
		DEBUG_VPRINT(LOG_MODULE, "Mounting %s on %s",
			store, MBOX_STORE_MOUNTPOINT);

		/* check that mountpoint exists and create it if it doesn't */
		if (stat(MBOX_STORE_MOUNTPOINT, &st) == -1) {
			mkdir_p(MBOX_STORE_MOUNTPOINT,
				S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		}

		/* check that the video directory exists in the store
		 * and create it if it doesn't */
		if (stat(MBOX_STORE_VIDEO, &st) == -1) {
			/* mount the store partition */
			if (mount(store, MBOX_STORE_MOUNTPOINT, "ext4", 0, "") == -1) {
				LOG_VPRINT_ERROR("Could not mount proc: %s",
					strerror(errno));
				return -1;
			}
		} else {
			if (!S_ISDIR(st.st_mode)) {
				LOG_VPRINT_ERROR("'%s' exists but it's not a directory!",
					MBOX_STORE_VIDEO);
				return -1;
			}
		}

		/* check that the video directory exists in the store
		 * and create it if it doesn't */
		if (stat(MBOX_STORE_VIDEO, &st) == -1) {
			if (mkdir_p(MBOX_STORE_VIDEO, S_IRWXU | S_IRWXG | S_IRWXO) == -1) {
				LOG_VPRINT_ERROR("Could not create 'Video' directory!: %s",
					strerror(errno));
				return -1;
			}
		} else {
			if (!S_ISDIR(st.st_mode)) {
				LOG_VPRINT_ERROR("'%s' exists but it's not a directory!",
					MBOX_STORE_VIDEO);
				return -1;
			}
		}

		/* check that the video directory exists in the store
		 * and create it if it doesn't */
		if (stat(MBOX_STORE_AUDIO, &st) == -1) {
			if (mkdir_p(MBOX_STORE_AUDIO, S_IRWXU | S_IRWXG | S_IRWXO) == -1) {
				LOG_VPRINT_ERROR("Could not create 'Audio' directory!: %s",
					strerror(errno));
				return -1;
			}
		} else {
			if (!S_ISDIR(st.st_mode)) {
				LOG_VPRINT_ERROR("'%s' exists but it's not a directory!",
					MBOX_STORE_VIDEO);
				return -1;
			}
		}

	}

	/* create the library database if it doesn't exist */
	if (mbox_library_create_db_if_not_exist()) {
		LOG_PRINT_ERROR("Could not create database!");
		return -1;
	}

	/* initialize watch list */
	LIST_INIT(&local_inotify_watches);

	/* initialize inotify */
	if ((local_inotify_fd = inotify_init1(IN_CLOEXEC)) == -1) {
		LOG_VPRINT_ERROR("Could not initialize inotify: %s",
			strerror(errno));
		return -1;
	}

	/* add watch */
	if (mbox_library_local_add_watch(MBOX_STORE_AUDIO) == -1) {
		LOG_VPRINT_ERROR("Could not watch directory '%s': %s",
			MBOX_STORE_AUDIO, strerror(errno));
		return -1;
	}
	if (mbox_library_local_add_watch(MBOX_STORE_VIDEO) == -1) {
		LOG_VPRINT_ERROR("Could not watch directory '%s': %s",
			MBOX_STORE_VIDEO, strerror(errno));
		return -1;
	}

	/* start inotify thread */
	if (pthread_create(&local_inotify_thread, NULL,
		mbox_library_local_inotify, NULL) != 0) {
		abort();
	}

	return 0;
}


static void
mbox_library_local_shutdown(void)
{
	struct mbox_library_local_watchdir *watch_dir;

	local_inotify_quit = 1;
	pthread_kill(local_inotify_thread, SIGUSR1);
	pthread_join(local_inotify_thread, NULL);

	/* remove all file watches */
	LIST_FOREACH_SAFE(struct mbox_library_local_watchdir*,
		watch_dir, &local_inotify_watches, {
		LIST_REMOVE(watch_dir);
		inotify_rm_watch(local_inotify_fd, watch_dir->watch_fd);
		free(watch_dir->path);
		free(watch_dir);
	});

	if (local_inotify_fd != -1) {
		close(local_inotify_fd);
		local_inotify_fd = -1;
	}

	if (store != NULL) {
		umount(store);
	}
}


/**
 * Gets the list of watched directories.
 */
const char **
mbox_library_watchdirs(void)
{
	static const char * watchdirs[] = {
		MBOX_STORE_AUDIO,
		MBOX_STORE_VIDEO,
		NULL
	};
	return watchdirs;
}


/**
 * Initialize the library backend.
 */
int
mbox_library_init(void)
{
	int argc, i, launch_avmount = 1, launch_mediatomb = 1, ret = -1;
	const char **argv;
	char exe_path_mem[255];
	char *exe_path = exe_path_mem;
	char *avmount_logfile = NULL;
	int config_setup = 0;
	struct stat st;

	DEBUG_PRINT(LOG_MODULE, "Starting library backend");

	/* parse command line arguments */
	for (i = 0, argv = avbox_application_args(&argc); i < argc; i++) {
		DEBUG_VPRINT(LOG_MODULE, "Got argument: '%s'",
			argv[i]);

		if (!strncmp(argv[i], "--store=", 8)) {
			if ((store = strdup(argv[i] + 8)) == NULL) {
				ASSERT(errno == ENOMEM);
				goto end;
			}
		} else if (!strcmp(argv[i], "--no-avmount")) {
			launch_avmount = 0;
		} else if (!strcmp(argv[i], "--no-mediatomb")) {
			launch_mediatomb = 0;
		}
	}

	/* Figure out which config file template to use.
	 * If we're running on the build directory we use
	 * the ones in the source tree. Otherwise use the
	 * installed ones */
	if (mb_getexepath(exe_path, sizeof(exe_path_mem)) == -1) {
		LOG_VPRINT(LOGLEVEL_ERROR, "library-backend", "Could not get executable path: %s",
			strerror(errno));
		exe_path[0] = '\0';

	} else {
		char *conf_path;
		exe_path = dirname(exe_path);

		DEBUG_VPRINT("library-backend", "Executable image path: %s",
			exe_path);

		if ((conf_path = malloc((strlen(exe_path) + 25 + 1) * sizeof(char))) != NULL) {
			struct stat st;
			strcpy(conf_path, exe_path);
			strcat(conf_path, "/res/mediatomb/config.xml");
			if (stat(conf_path, &st) == 0) {
				DEBUG_VPRINT("library-backend", "Config template found at: %s",
					conf_path);
				strcpy(conf_path, exe_path);
				strcat(conf_path, "/res/mediatomb");
				if ((mediatomb_home = mb_library_backend_mediaboxsetup(conf_path)) == NULL) {
					LOG_PRINT(LOGLEVEL_ERROR, "library-backend",
						"Could not setup mediatomb config.");
						goto end;
				}
				config_setup = 1;
			} else {
				DEBUG_VPRINT("library-backend", "Config template not found: %s",
					conf_path);
			}
			free(conf_path);
		}
	}

	if (!config_setup) {
		if ((mediatomb_home = mb_library_backend_mediaboxsetup(
			STRINGIZE(DATADIR) "/mediabox/mediatomb")) == NULL) {
			LOG_PRINT(LOGLEVEL_ERROR, "library-backend",
				"Could not setup mediatomb config (2).");
			goto end;
		}
	}

	ASSERT(mediatomb_home != NULL);

	/* check that we have permission to write to DEFAULT_LOGFILE
	 * before trying */
	if (stat(DEFAULT_LOGFILE, &st) == 0) {
		if (access(DEFAULT_LOGFILE, W_OK) == 0) {
			avmount_logfile = DEFAULT_LOGFILE;
		}
	} else {
		if (stat("/var/log", &st) == 0) {
			if (access("/var/log", W_OK) == 0) {
				avmount_logfile = DEFAULT_LOGFILE;
			}
		}
	}

	/* If we can't write to the log file (probably because
	 * we're running as a regular user for debugging) then
	 * write the log to /tmp */
	if (avmount_logfile == NULL) {
		avmount_logfile = "/tmp/avmount-mediabox.log";
	}

	/* initialize a linked list to hold mediatomb instances */
	LIST_INIT(&mediatomb_instances);

	/* launch the avmount process */
	if (launch_avmount) {
		struct stat st;

		char * const avargs[] =
		{
			AVMOUNT_BIN,
#ifndef NDEBUG
			"-d",
#endif
			"-l",
			avmount_logfile,
			"--lobind",
			"-f",
			"-p",
			"49152",
			"-o",
			"allow_other",
			AVMOUNT_MOUNTPOINT,
			NULL
		};

		/* if the mountpoint doesn't exist, create it */
		if (stat(AVMOUNT_MOUNTPOINT, &st)) {
			DEBUG_PRINT("library-backend", AVMOUNT_MOUNTPOINT " not found.Creating");
			if (mkdir_p(AVMOUNT_MOUNTPOINT, S_IRWXU)) {
				DEBUG_VPRINT("library-backend", "Could not create mountpoint "
					AVMOUNT_MOUNTPOINT ": %s", strerror(errno));
				goto end;
			}
		}


		DEBUG_VPRINT("library-backend", "Running " AVMOUNT_BIN " -l %s --lobind -f "
			"-p 49152 -o allow_other " AVMOUNT_MOUNTPOINT, avmount_logfile);

		if ((avmount_process_id = avbox_process_start(AVMOUNT_BIN, (const char **) avargs,
			AVBOX_PROCESS_AUTORESTART | AVBOX_PROCESS_AUTORESTART_ALWAYS |
			AVBOX_PROCESS_NICE | AVBOX_PROCESS_IONICE_IDLE | AVBOX_PROCESS_SUPERUSER,
			"avmount", avbox_avmount_exit, NULL)) == -1) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "library-backend", "Could not start avmount daemon");
			goto end;
		}
	}

	/* initialize local provider */
	if (mbox_library_local_init() == -1) {
		LOG_VPRINT_ERROR("Could not start local library provider: %s",
			strerror(errno));
		goto end;
	}

	/* launch a mediabox process for each interface */
	if (launch_mediatomb) {
		struct mt_init_state state;
		state.port = 49163;
		state.err = 0;
		state.gotone = 0;
		ifaceutil_enumifaces(mb_library_backend_startmediatomb, &state);
		if (state.err != 0) {
			LOG_PRINT_ERROR("An error occurred while launching mediatomb!");
			goto end;
		}
	}

#if defined(ENABLE_DVD) || defined(ENABLE_USB)
	if ((udev = udev_new()) == NULL) {
		LOG_PRINT_ERROR("Could not create udev intance!");
	}
#endif

	ret = 0;
end:
	return ret;
}


/**
 * Shutdown the library backend.
 */
void
mbox_library_shutdown(void)
{
	struct mb_mediatomb_inst *inst;
	LIST_FOREACH_SAFE(struct mb_mediatomb_inst*, inst, &mediatomb_instances, {
		avbox_process_stop(inst->procid);
		LIST_REMOVE(inst);
	});
	if (avmount_process_id != -1) {
		avbox_process_stop(avmount_process_id);
	}

	mbox_library_local_shutdown();

	if (store != NULL) {
		free(store);
	}

#if defined(ENABLE_DVD) || defined(ENABLE_USB)
	ASSERT(udev != NULL);
	udev_unref(udev);
	udev = NULL;
#endif
}
