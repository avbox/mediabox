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

#define LOG_MODULE "settings"

#include <libavbox/avbox.h>


#define DEFAULT_HOSTNAME	("mediabox-v0")


static pthread_mutex_t dblock;


#define SQLBUFLEN	(1024)


/**
 * Callback function to get getstring() result.
 */
static int
avbox_settings_getstring_callback(void *data,
	int argc, char **argv, char **azColName)
{
	char **value = (char**) data;
	ASSERT(argc <= 1);
	ASSERT(data != NULL);

	if ((*value = strdup(argv[0])) == NULL) {
		LOG_VPRINT_ERROR("Could not strdup() query result '%s'",
			argv[0]);
	}
	return 0;
}


/**
 * Gets the value of a setting. The result must
 * be freed with free().
 */
char *
avbox_settings_getstring(const char * const key)
{
	int res;
	char *value = NULL, sql[SQLBUFLEN];
	sqlite3 *db = NULL;
	char *safekey, *filename = NULL;

	DEBUG_VPRINT(LOG_MODULE, "Entering settings_getstring(\"%s\")",
		key);

	ASSERT(key != NULL);

	if ((safekey = avbox_dbutil_escapesql(key)) == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	if ((filename = avbox_dbutil_getdbfile("settings.db")) == NULL) {
		ASSERT(errno == ENOMEM);
		free(safekey);
		return NULL;
	}

	/* build sql statement */
	snprintf(sql, SQLBUFLEN, "SELECT value FROM settings WHERE key = '%s' LIMIT 1;",
		safekey);

	/* open db and exec statement */
	if ((res = sqlite3_open_v2(filename,
		&db, SQLITE_OPEN_READONLY, NULL)) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Could not open database '%s': %s (%d)",
			filename, sqlite3_errmsg(db), res);
		goto end;
	}
	if ((res = sqlite3_exec(db, sql,
		&avbox_settings_getstring_callback, &value, NULL)) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Query '%s' failed (%d)",
			sql, res);
		goto end;
	}
end:
	if (safekey != NULL) {
		free(safekey);
	}
	if (filename != NULL) {
		free(filename);
	}
	if (db != NULL) {
		sqlite3_close(db);
	}
	return value;
}


/**
 * Sets a string value on the settings database.
 */
int
avbox_settings_setstring(const char * const key,
	const char * const value)
{
	int ret = -1, res;
	sqlite3 *db = NULL;
	char *existing = NULL, *filename = NULL;
	char sql[SQLBUFLEN];

	DEBUG_VPRINT(LOG_MODULE, "Entering settings_setstring(\"%s\", \"%s\")",
		key, value);

	ASSERT(key != NULL);

	char * const safekey = avbox_dbutil_escapesql(key);
	char * const safevalue = avbox_dbutil_escapesql(value);
	if (safekey == NULL || safevalue == NULL) {
		errno = ENOMEM;
		goto end;
	}

	pthread_mutex_lock(&dblock);

	existing = avbox_settings_getstring(key);

	if (existing == NULL) {
		/* doesn't exist and value is NULL */
		if (value == NULL) {
			goto end;
		}
		snprintf(sql, SQLBUFLEN,
			"INSERT INTO settings (key, value) VALUES ('%s', '%s');",
			safekey, safevalue);
	} else {
		if (value == NULL) {
			snprintf(sql, SQLBUFLEN,
				"DELETE FROM settings WHERE key = '%s';",
				safekey);
		} else {
			snprintf(sql, SQLBUFLEN,
				"UPDATE settings SET value = '%s' WHERE key = '%s';",
				safevalue, safekey);
		}
	}

	if ((filename = avbox_dbutil_getdbfile("settings.db")) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}

	/* open db and exec query */
	if ((res = sqlite3_open_v2(filename, &db,
		SQLITE_OPEN_READWRITE, NULL)) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Could not open database '%s': %s (%d)",
			filename, sqlite3_errmsg(db), res);
	}
	if ((res = sqlite3_exec(db, sql, NULL, NULL, NULL)) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Could not exec query '%s' (%d)",
			sql, res);
		goto end;
	}

	ret = 0;
end:
	if (safekey != NULL) {
		free(safekey);
	}
	if (safevalue != NULL) {
		free(safevalue);
	}
	if (existing != NULL) {
		free(existing);
	}
	if (db != NULL) {
		sqlite3_close(db);
	}
	if (filename != NULL) {
		free(filename);
	}
	pthread_mutex_unlock(&dblock);
	return ret;
}


/**
 * Gets a boolean value from the settings database.
 */
int
avbox_settings_getbool(const char * const key)
{
	int ret = 0;
	char *str;

	DEBUG_VPRINT("settings", "Entering settings_getbool(\"%s\")",
		key);

	str = avbox_settings_getstring(key);
	if (str == NULL || !strcmp(str, "0")) {
		goto end;
	} else if (!strcmp(str, "1")) {
		ret = 1;
		goto end;
	} else {
		LOG_VPRINT_ERROR("BUG: Boolean value set to '%s'!",
			str);
		abort();
	}
end:
	if (str != NULL) {
		free(str);
	}
	return ret;
}


/**
 * Sets a boolean value on the settings database.
 */
int
avbox_settings_setbool(const char * const key, const int value)
{
	DEBUG_VPRINT(LOG_MODULE, "Entering settings_setbool(\"%s\", %i)",
		key, value);

	ASSERT(value == 0 || value == 1);
	if (value == 0) {
		return avbox_settings_setstring(key, "0");
	} else {
		return avbox_settings_setstring(key, "1");
	}
}


/**
 * Sets an integer value on the settings database.
 */
int
avbox_settings_setint(const char * const key, const int value)
{
	char svalue[32];
	DEBUG_VPRINT(LOG_MODULE, "Entering settings_setint(\"%s\", %i)",
		key, value);
	sprintf(svalue, "%d", value);
	return avbox_settings_setstring(key, svalue);
}


/**
 * Gets an integer value from the settings database.
 */
int
avbox_settings_getint(const char * key, const int defvalue)
{
	char *svalue;
	int ret = defvalue;
	DEBUG_VPRINT(LOG_MODULE, "Entering settings_getint(\"%s\", %d)",
		key, defvalue);
	if ((svalue = avbox_settings_getstring(key)) != NULL) {
		ret = atoi(svalue);
		free(svalue);
	}
	return ret;
}


/**
 * Creates the settings database.
 */
static int
avbox_settings_createdb()
{
	int ret = -1, res;
	sqlite3 *db = NULL;
	char *filename = NULL;
	const char *sql =
		"CREATE TABLE settings ("
		"key TEXT,"
		"value TEXT"
		")";

	DEBUG_PRINT("settings", "Creating settings database");

	pthread_mutex_lock(&dblock);

	if ((filename = avbox_dbutil_getdbfile("settings.db")) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}

	if (sqlite3_open_v2(filename, &db,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Could not open database '%s': %s",
			filename, sqlite3_errmsg(db));
		goto end;
	}
	if ((res = sqlite3_exec(db, sql, NULL, NULL, NULL)) != SQLITE_OK) {
		LOG_VPRINT_ERROR("SQL Query: '%s' failed (%d)!", sql, res);
		goto end;
	}

	/* set some defaults */
	if (avbox_settings_setstring("hostname", DEFAULT_HOSTNAME) == -1) {
		LOG_VPRINT_ERROR("settings_setstring() failed: %s",
			strerror(errno));
	}
	if (avbox_settings_setbool("use_dhcp", 1) == -1) {
		LOG_VPRINT_ERROR("settings_setbool() failed: %s",
			strerror(errno));
	}

	ret = 0;
end:
	if (db != NULL) {
		sqlite3_close(db);
	}
	if (filename != NULL) {
		free(filename);
	}
	pthread_mutex_unlock(&dblock);
	return ret;
}


/**
 * Initialize settings database.
 */
int
avbox_settings_init()
{
	struct stat st;
	char *filename;
	pthread_mutexattr_t lockattr;

	DEBUG_PRINT("settings", "Initializing settings database");

	/* initialize mutex */
	pthread_mutexattr_init(&lockattr);
	pthread_mutexattr_settype(&lockattr, PTHREAD_MUTEX_RECURSIVE);
	if (pthread_mutex_init(&dblock, &lockattr) != 0) {
		LOG_PRINT_ERROR("Could not initialize mutex!");
	}

	if ((filename = avbox_dbutil_getdbfile("settings.db")) == NULL) {
		LOG_VPRINT_ERROR("Could not create db filename: %s",
			strerror(errno));
		return -1;
	}

	/* check that the database exists and we can
	 * access it. If it's not try to create it */
	while (stat(filename, &st) == -1 ||
		access(filename, R_OK|W_OK) == -1) {
		if (avbox_settings_createdb() == -1) {
			LOG_VPRINT_ERROR("Could not create database: %s (%d)",
				strerror(errno), errno);
			free(filename);
			return -1;
		}
	}

	free(filename);

	return 0;
}


/**
 * Shutdown settings database.
 */
void
avbox_settings_shutdown()
{
}
