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


#ifdef HAVE_CONFIG_H
#       include <libavbox/config.h>
#endif

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sqlite3.h>

#define LOG_MODULE "db_util"

#include <libavbox/avbox.h>


char *
avbox_dbutil_getdbfile(const char * const filename)
{
	char *statedir = NULL, *statefile, *ret = NULL;

	/* get the state directory (usually /var/lib/mediabox) */
	if ((statedir = getstatedir()) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}

	const size_t buflen = strlen(statedir) + 1 + strlen(filename) + 1;

	if ((statefile = malloc(buflen)) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}

	/* copy path to buffer */
	strcpy(statefile, statedir);
	strcat(statefile, "/");
	strcat(statefile, filename);
	ret = statefile;

	/* DEBUG_VPRINT(LOG_MODULE, "avbox_dbutil_getdbfile(\"%s\") -> \"%s\"",
		filename, statefile); */
end:
	if (statedir != NULL) {
		free(statedir);
	}
	return ret;
}


char *
avbox_dbutil_escapesql(const char * const sql)
{
	char *safesql, *psafesql;
	const char *psql;
	size_t safesql_len = 0;

	/* first calcalate the length of the escaped statement */
	psql = sql;
	while (*psql != '\0') {
		if (*psql == '\'') {
			safesql_len += 2;
		} else {
			safesql_len++;
		}
		psql++;
	}

	/* allocate memory for it */
	if ((safesql = malloc((safesql_len + 1) * sizeof(char))) == NULL) {
		return NULL;
	}

	/* and copy it doubling the quotes */
	psql = sql;
	psafesql = safesql;
	while (*psql != '\0') {
		if (*psql == '\'') {
			*psafesql++ = '\'';
		}
		*psafesql++ = *psql++;
	}
	*psafesql = '\0';
	return safesql;
}
