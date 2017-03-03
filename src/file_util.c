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
				close(fd);
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
		if (path_tmp_ptr[-1] != '/') {
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
mb_getdatadir(char *buf, size_t bufsize)
{
	char exe_path_mem[255];
	char *exe_path = exe_path_mem;

	if (mb_getexepath(exe_path, sizeof(exe_path_mem)) == -1) {
		LOG_VPRINT(LOGLEVEL_ERROR, "file-util", "Could not get executable path: %s",
			strerror(errno));
		strncpy(buf, DATADIR, bufsize);
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
				if (strlen(DATADIR) < bufsize) {
					strcpy(buf, DATADIR);
					free(conf_path);
					return buf;
				}
			}
			free(conf_path);
		}
	}
	return NULL;
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
