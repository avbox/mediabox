/**
 * avbox - Toolkit for Embedded Multimedia Applications
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


#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <libgen.h>

#define LOG_MODULE "file-util"

#include "debug.h"
#include "log.h"
#include "string_util.h"
#include "proc_util.h"


#define STRINGIZE2(x)	#x
#define STRINGIZE(x)	STRINGIZE2(x)


/**
 * closefrom() -- Close all file descriptors >= fd_max.
 */
int
closefrom(int fd_max)
{
	DIR *dir;
	struct dirent *ent;
	int fd;

	/* Should work on Linux, Solaris, AIX, Cygwin, NetBSD */

	if ((dir = opendir("/proc/self/fd")) == NULL) {
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		if (strisdigit(ent->d_name)) {
			fd = atoi(ent->d_name);
			if (fd >= fd_max) {
				/* valgrind opens file descriptors above 1024.
				 * TODO: Find out how to check that they're actually
				 * valgrind files */
				if (fd < 1024) {
					close(fd);
				}
			}
		}
	}

	closedir(dir);

	return 0;
}


/**
 * Copies a file
 */
int
cp(const char *src, const char *dst)
{
	int fdr, fdw, ret = -1;
	struct stat st;

	if (stat(src, &st) == 0) {
		if ((fdr = open(src, O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)) != -1) {
			if ((fdw = open(dst, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)) != -1) {
				if (sendfile(fdw, fdr, 0, st.st_size) != -1) {
					ret = 0;
				} else {
					LOG_VPRINT_ERROR("sendfile() failed: %s", strerror(errno));
				}
				close(fdw);
			} else {
				LOG_VPRINT_ERROR("Could not open '%s': %s", dst, strerror(errno));
			}
			close(fdr);
		} else {
			LOG_VPRINT_ERROR("Could not open '%s': %s", dst, strerror(errno));
		}
	}
	return ret;
}


/**
 * Create a directory recursively.
 */
int
mkdir_p(const char * const path, mode_t mode)
{
	char path_tmp[255] = "/";
	const char *path_tmp_ubound = &path_tmp[254];
	const char *path_ptr = path;
	char *path_tmp_ptr = path_tmp;
	struct stat st;

	while (*path_ptr != '\0') {
		if (path_tmp_ptr >= path_tmp_ubound) {
			LOG_PRINT_ERROR("mkdir_p cannot handle paths greater than 254 chars.");
			return -1;
		}
		if (path_tmp_ptr == path_tmp || path_tmp_ptr[-1] != '/') {
			*path_tmp_ptr++ = '/';
		}
		while (*path_ptr != '\0') {
			if (*path_ptr == '/') {
				path_ptr++;
				*path_tmp_ptr = '\0';
				break;
			}
			if (path_tmp_ptr >= path_tmp_ubound) {
				LOG_PRINT_ERROR("mkdir_p cannot handle paths greater than 254 chars.");
				return -1;
			}
			*path_tmp_ptr++ = *path_ptr++;
		}
		if (stat(path_tmp, &st) == -1) {
			if (mkdir(path_tmp, mode) == -1) {
				return -1;
			}
		}
	}
	if (stat(path, &st) == -1) {
		LOG_PRINT_ERROR("mkdir_p: failed");
		return -1;
	}
	return 0;
}


/**
 * Get the data directory. If we're running from
 * the source directory use res/ as the data directory.
 * Otherwise use the configured DATADIR.
 */
char *
avbox_getdatadir(char *buf, size_t bufsize)
{
	char exe_path_mem[255];
	char *exe_path = exe_path_mem;

	if (mb_getexepath(exe_path, sizeof(exe_path_mem)) == -1) {
		LOG_VPRINT(LOGLEVEL_ERROR, "file-util", "Could not get executable path: %s",
			strerror(errno));
		strncpy(buf, STRINGIZE(DATADIR), bufsize);
		return buf;

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
				DEBUG_VPRINT("file-util", "Config template found at: %s",
					conf_path);
				strcpy(conf_path, exe_path);
				strcat(conf_path, "/res");
				if (strlen(conf_path) < bufsize) {
					strcpy(buf, conf_path);
					free(conf_path);
					return buf;
				}
			} else {
				DEBUG_VPRINT("library-backend", "Config template not found: %s",
					conf_path);
				if (strlen(STRINGIZE(DATADIR)) < bufsize) {
					strcpy(buf, STRINGIZE(DATADIR));
					free(conf_path);
					return buf;
				}
			}
			free(conf_path);
		}
	}
	return NULL;
}

char*
mb_getdatadir(char *buf, size_t bufsize)
{
	return avbox_getdatadir(buf, bufsize);
}

char*
avbox_get_resource(const char*const res, int *sz)
{
	char *buf, *buf_p;
	char fname[1024];
	struct stat st;
	int fd, bytes_to_read, bytes_read;

	avbox_getdatadir(fname, sizeof(fname));
	strcat(fname, "/");
	strcat(fname, res);

	DEBUG_VPRINT(LOG_MODULE, "Getting resource: %s",
		fname);

	/* stat file info */
	if (stat(fname, &st) == -1) {
		LOG_VPRINT_ERROR("Could not stat resource (%s): %s",
			fname, strerror(errno));
		return NULL;
	}

	/* allocate buffer for resource data */
	if ((buf = malloc(st.st_size + 1)) == NULL) {
		LOG_VPRINT_ERROR("Could not allocate memory for resource (%s|size=%li): %s",
			fname, st.st_size, strerror(errno));
		return NULL;
	}

	if ((fd = open(fname, O_RDONLY)) == -1) {
		LOG_VPRINT_ERROR("Could not open resource file (%s): %s",
			fname, strerror(errno));
		free(buf);
		return NULL;
	}

	/* read the resource into memory */
	buf_p = buf;
	buf[st.st_size] = '\0';
	bytes_to_read = st.st_size;
	ASSERT(bytes_to_read > 0);
	while (bytes_to_read > 0) {
		if ((bytes_read = read(fd, buf_p, bytes_to_read)) < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
		}
		bytes_to_read -= bytes_read;
		buf_p += bytes_read;
		ASSERT(bytes_to_read >= 0);
	}

	if (sz != NULL) {
		*sz = st.st_size;
	}

	close(fd);
	return buf;
}


/**
 * Gets the state directory (usually /var/lib/mediabox)
 */
char *
getstatedir()
{
	struct stat st;

	/* attempt to create the directory if it
	 * doesn't exists */
	mkdir_p(STRINGIZE(LOCALSTATEDIR) "/lib/mediabox", S_IRWXU);

	/* if it still doesn't exists or we can't access it
	 * then use something else */
	if (stat(STRINGIZE(LOCALSTATEDIR) "/lib/mediabox", &st) == -1 ||
		access(STRINGIZE(LOCALSTATEDIR) "/lib/mediabox", R_OK|W_OK) == -1) {
		DEBUG_VPRINT("file-util", "Could not access '%s': %s",
			STRINGIZE(LOCALSTATEDIR) "/mediabox", strerror(errno));
		char *envhome = getenv("HOME");
		char home[PATH_MAX];
		if (envhome == NULL) {
			return NULL;
		}

		strncpy(home, envhome, PATH_MAX);
		strncat(home, "/.mediabox", PATH_MAX);
		mkdir_p(home, S_IRWXU);
		return strdup(home);
	} else {
		/* otherwise just return it */
		return strdup(STRINGIZE(LOCALSTATEDIR) "/lib/mediabox");
	}
}


/**
 * Copies a file from ifilename to ofilename replacing
 * all occurrences of match with replace.
 */
int
frep(const char * const ifilename,
	const char * ofilename,
	const char * const match[],
	const char * const replace[])
{
	FILE *fin, *fout;
	char *lineptr = NULL;
	size_t i_n = 0;

	if (ofilename == NULL) {
		ofilename = ifilename;
	}

	/* Open both files. If they both point to the same
	 * file unlink() it after opening it as input */
	if ((fin = fopen(ifilename, "r")) == NULL) {
		return -1;
	}
	if (ifilename == ofilename || !strcmp(ifilename, ofilename)) {
		unlink(ifilename);
	}
	if ((fout = fopen(ofilename, "w")) == NULL) {
		fclose(fin);
		return -1;
	}

	while (getline(&lineptr, &i_n, fin) > 0) {
		const char * const * pmatch = &match[0];
		const char * const * preplace = &replace[0];
		char * matchptr = NULL;

		/* first we find a match string that matches the
		 * current line. This limits this implementation to
		 * replacing one match per line */
		while (*pmatch != NULL) {
			if ((matchptr = strstr(lineptr, *pmatch)) != NULL) {
				break;
			}
			pmatch++;
			preplace++;
		}
		if (matchptr != NULL) {
			fwrite(lineptr, matchptr - lineptr, sizeof(char), fout);
			fwrite(*preplace, strlen(*preplace), sizeof(char), fout);
			matchptr += strlen(*pmatch);
			fwrite(matchptr, strlen(matchptr), sizeof(char), fout);
		} else {
			fwrite(lineptr, strlen(lineptr), sizeof(char), fout);
		}

	}

	/* cleanup and return */
	if (lineptr != NULL) {
		free(lineptr);
	}
	fclose(fout);
	fclose(fin);
	return 0;
}


/**
 * Copy a file from the data directory.
 */
int
cpdata(const char *relsrc, const char *dst)
{
	return -1;
}
