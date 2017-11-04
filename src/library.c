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
#include "config.h"
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

#define LOG_MODULE "library-backend"

#include "lib/log.h"
#include "lib/debug.h"
#include "lib/process.h"
#include "lib/iface_util.h"
#include "lib/linkedlist.h"
#include "lib/proc_util.h"
#include "lib/file_util.h"
#include "library.h"


#define MEDIATOMB_BIN "/usr/bin/mediatomb"
#define MEDIATOMB_RUN "/tmp/mediabox/mediatomb"
#define MEDIATOMB_VAR "/var/mediabox/mediatomb"
#define AVMOUNT_BIN "/usr/bin/avmount"
#define AVMOUNT_MOUNTPOINT "/media/UPnP"
#define DEFAULT_LOGFILE "/var/log/avmount-mediabox.log"
#define FUSERMOUNT_BIN "/usr/bin/fusermount"

#define UPNP_ROOT	"/media/UPnP"

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
	char *home;
};

LIST_DECLARE_STATIC(mediatomb_instances);
static int avmount_process_id = -1;


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
	snprintf(homedir, sizeof(homedir), "%s.%s", state->home, iface_name);
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
		AVBOX_PROCESS_SUPERUSER/* | AVBOX_PROCESS_STDOUT_LOG | AVBOX_PROCESS_STDERR_LOG */,
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


/**
 * Open a library directory
 */
struct mbox_library_dir *
mbox_library_opendir(const char * const path)
{
	struct mbox_library_dir *dir;

	if ((dir = malloc(sizeof(struct mbox_library_dir))) == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	if ((dir->path = strdup(path)) == NULL) {
		errno = ENOMEM;
		free(dir);
		return NULL;
	}

	if (!strcmp("/", path)) {
		dir->type = MBOX_LIBRARY_DIRTYPE_ROOT;
		dir->state.rootdir.ptr = NULL;

		LIST_INIT(&dir->state.rootdir.entries);

		mbox_library_adddirent("Local Files", "/local", 1, &dir->state.rootdir.entries);
		mbox_library_adddirent("UPnP Devices", "/upnp", 1, &dir->state.rootdir.entries);
		mbox_library_adddirent("TV Tunners", "/tv", 1, &dir->state.rootdir.entries);
		mbox_library_adddirent("DVD", "/dvd", 1, &dir->state.rootdir.entries);
		mbox_library_adddirent("Bluetooth Devices", "/bluetooth", 1, &dir->state.rootdir.entries);

	} else if (!strncmp("/local", path, 6)) {
		dir->type = MBOX_LIBRARY_DIRTYPE_LOCAL;
		dir->state.emptydir.read = 0;
	} else if (!strncmp("/upnp", path, 5)) {
		char *rpath;
		char resolved_path[PATH_MAX];
		const char * const ppath = (path + 5);
		const int path_len = strlen(path + 5);

		if ((rpath = malloc(sizeof(UPNP_ROOT) + path_len + 2)) == NULL) {
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
			free(dir);
			return NULL;
		}

		if ((dir->state.upnpdir.path = strdup(resolved_path)) == NULL) {
			free(rpath);
			free(dir);
		}

		dir->type = MBOX_LIBRARY_DIRTYPE_UPNP;
		dir->state.upnpdir.dir = opendir(rpath);
		if (dir->state.upnpdir.dir == NULL) {
			DEBUG_VPRINT(LOG_MODULE, "opendir(\"%s\") failed: %s",
				resolved_path, strerror(errno));
			free(rpath);
			free(dir);
		}

		free(rpath);

	} else if (!strncmp("/dvd", path, 4)) {
		dir->type = MBOX_LIBRARY_DIRTYPE_DVD;
		dir->state.emptydir.read = 0;
	} else if (!strncmp("/bluetooth", path, 10)) {
		dir->type = MBOX_LIBRARY_DIRTYPE_BLUETOOTH;
		dir->state.emptydir.read = 0;
	} else if (!strncmp("/tv", path, 3)) {
		dir->type = MBOX_LIBRARY_DIRTYPE_TV;
		dir->state.emptydir.read = 0;
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
	case MBOX_LIBRARY_DIRTYPE_LOCAL:
	case MBOX_LIBRARY_DIRTYPE_TV:
	case MBOX_LIBRARY_DIRTYPE_DVD:
	case MBOX_LIBRARY_DIRTYPE_BLUETOOTH:
	{
		struct mbox_library_dirent *ent;

		/* if we already read this directory then return EOF */
		if (dir->state.emptydir.read) {
			return NULL;
		}

		/* mark the directory handle as read */
		dir->state.emptydir.read = 1;

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
	case MBOX_LIBRARY_DIRTYPE_UPNP:
	{
		ASSERT(dir->state.upnpdir.path != NULL);
		free(dir->state.upnpdir.path);
		closedir(dir->state.upnpdir.dir);
		break;
	}
	case MBOX_LIBRARY_DIRTYPE_LOCAL:
	case MBOX_LIBRARY_DIRTYPE_TV:
	case MBOX_LIBRARY_DIRTYPE_DVD:
	case MBOX_LIBRARY_DIRTYPE_BLUETOOTH:
	{
		break;
	}
	default:
		abort();
	}

	free(dir->path);
	free(dir);
}


/**
 * Initialize the library backend.
 */
int
mbox_library_init(const int launch_avmount,
	const int launch_mediatomb)
{
	char exe_path_mem[255];
	char *exe_path = exe_path_mem;
	char *avmount_logfile = NULL;
	char *mt_home = NULL;
	int config_setup = 0;
	struct stat st;

	DEBUG_PRINT("library-backend", "Starting library backend");

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
				if ((mt_home = mb_library_backend_mediaboxsetup(conf_path)) == NULL) {
					LOG_PRINT(LOGLEVEL_ERROR, "library-backend",
						"Could not setup mediatomb config.");
					return -1;
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
		if ((mt_home = mb_library_backend_mediaboxsetup(DATADIR "/mediabox/mediatomb")) == NULL) {
			LOG_PRINT(LOGLEVEL_ERROR, "library-backend",
				"Could not setup mediatomb config (2).");
			return -1;
		}
	}

	ASSERT(mt_home != NULL);

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

	/* launch a mediabox process for each interface */
	if (launch_mediatomb) {
		struct mt_init_state state;
		state.port = 49163;
		state.home = mt_home;
		state.err = 0;
		state.gotone = 0;
		ifaceutil_enumifaces(mb_library_backend_startmediatomb, &state);
		free(mt_home);
		if (state.err != 0) {
			LOG_PRINT_ERROR("An error occurred while launching mediatomb!");
			return -1;
		}
	}

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
				return -1;
			}
		}


		DEBUG_VPRINT("library-backend", "Running " AVMOUNT_BIN " -l %s --lobind -f "
			"-p 49152 -o allow_other " AVMOUNT_MOUNTPOINT, avmount_logfile);

		if ((avmount_process_id = avbox_process_start(AVMOUNT_BIN, (const char **) avargs,
			AVBOX_PROCESS_AUTORESTART | AVBOX_PROCESS_AUTORESTART_ALWAYS |
			AVBOX_PROCESS_NICE | AVBOX_PROCESS_IONICE_IDLE | AVBOX_PROCESS_SUPERUSER,
			"avmount", avbox_avmount_exit, NULL)) == -1) {
			LOG_PRINT(MB_LOGLEVEL_ERROR, "library-backend", "Could not start avmount daemon");
			return -1;
		}
	}

	return 0;
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
}
