#ifdef HAVE_CONFIG_H
#include "config.h"
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

#include "log.h"
#include "debug.h"
#include "file_util.h"

#define DEFAULT_HOSTNAME	("mediabox-v0")


static pthread_mutex_t dblock;


static char *
settings_getdbfile()
{
	static int done = 0;
	static char dbfile[PATH_MAX] = "";
	char *statedir = NULL, *ret = NULL;

	if (done) {
		return dbfile;
	}

	/* get the state directory (usually /var/lib/mediabox) */
	if ((statedir = getstatedir()) == NULL) {
		LOG_VPRINT_ERROR("Could not get state directory: %s",
			strerror(errno));
		goto end;
	}

	/* copy path to buffer */
	strncpy(dbfile, statedir, PATH_MAX);
	strncat(dbfile, "/settings.db", PATH_MAX);
	ret = dbfile;
	done = 1;

	DEBUG_VPRINT("settings", "Settings database: %s",
		dbfile);
end:
	if (statedir != NULL) {
		free(statedir);
	}
	return ret;

}


#define SQLBUFLEN	(1024)


static char *
settings_escapesql(const char * const sql)
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
	if ((safesql = malloc(safesql_len * sizeof(char))) == NULL) {
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


/**
 * Callback function to get getstring() result.
 */
static int
settings_getstring_callback(void *data,
	int argc, char **argv, char **azColName)
{
	char **value = (char**) data;
	assert(argc <= 1);
	assert(data != NULL);

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
settings_getstring(const char * const key)
{
	int res;
	char *value = NULL, sql[SQLBUFLEN];
	sqlite3 *db = NULL;

	DEBUG_VPRINT("settings", "Entering settings_getstring(\"%s\")",
		key);

	assert(key != NULL);

	char * const safekey = settings_escapesql(key);
	if (safekey == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	DEBUG_VPRINT("settings", "settings_getstring('%s')",
		key);

	/* build sql statement */
	snprintf(sql, SQLBUFLEN, "SELECT value FROM settings WHERE key = '%s' LIMIT 1;",
		safekey);

	/* open db and exec statement */
	if ((res = sqlite3_open_v2(settings_getdbfile(),
		&db, SQLITE_OPEN_READONLY, NULL)) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Could not open database '%s': %s (%d)",
			settings_getdbfile(), sqlite3_errmsg(db), res);
		goto end;
	}
	if ((res = sqlite3_exec(db, sql,
		&settings_getstring_callback, &value, NULL)) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Query '%s' failed (%d)",
			sql, res);
		goto end;
	}
end:
	if (safekey != NULL) {
		free(safekey);
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
settings_setstring(const char * const key,
	const char * const value)
{
	int ret = -1, res;
	sqlite3 *db = NULL;
	char *existing = NULL;
	char sql[SQLBUFLEN];

	DEBUG_VPRINT("settings", "Entering settings_setstring(\"%s\", \"%s\")",
		key, value);

	assert(key != NULL);

	char * const safekey = settings_escapesql(key);
	char * const safevalue = settings_escapesql(value);
	if (safekey == NULL || safevalue == NULL) {
		errno = ENOMEM;
		goto end;
	}

	pthread_mutex_lock(&dblock);

	existing = settings_getstring(key);

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

	/* open db and exec query */
	if ((res = sqlite3_open_v2(settings_getdbfile(), &db, SQLITE_OPEN_READWRITE, NULL)) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Could not open database '%s': %s (%d)",
			settings_getdbfile(), sqlite3_errmsg(db), res);
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
	pthread_mutex_unlock(&dblock);
	return ret;
}


/**
 * Gets a boolean value from the settings database.
 */
int
settings_getbool(const char * const key)
{
	int ret = 0;
	char *str;

	DEBUG_VPRINT("settings", "Entering settings_getbool(\"%s\")",
		key);

	str = settings_getstring(key);
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
settings_setbool(const char * const key, const int value)
{
	DEBUG_VPRINT("settings", "Entering settings_setbool(\"%s\", %i)",
		key, value);

	assert(value == 0 || value == 1);
	if (value == 0) {
		return settings_setstring(key, "0");
	} else {
		return settings_setstring(key, "1");
	}
}


/**
 * Creates the settings database.
 */
static int
settings_createdb()
{
	int ret = -1, res;
	sqlite3 *db = NULL;
	const char *sql =
		"CREATE TABLE settings ("
		"key TEXT,"
		"value TEXT"
		")";

	DEBUG_PRINT("settings", "Creating settings database");

	pthread_mutex_lock(&dblock);

	if (sqlite3_open_v2(settings_getdbfile(), &db,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
		LOG_VPRINT_ERROR("Could not open database '%s': %s",
			settings_getdbfile(), sqlite3_errmsg(db));
		goto end;
	}
	if ((res = sqlite3_exec(db, sql, NULL, NULL, NULL)) != SQLITE_OK) {
		LOG_VPRINT_ERROR("SQL Query: '%s' failed (%d)!", sql, res);
		goto end;
	}

	/* set some defaults */
	if (settings_setstring("hostname", DEFAULT_HOSTNAME) == -1) {
		LOG_VPRINT_ERROR("settings_setstring() failed: %s",
			strerror(errno));
	}
	if (settings_setbool("use_dhcp", 1) == -1) {
		LOG_VPRINT_ERROR("settings_setbool() failed: %s",
			strerror(errno));
	}

	ret = 0;
end:
	if (db != NULL) {
		sqlite3_close(db);
	}
	pthread_mutex_unlock(&dblock);
	return ret;
}


/**
 * Initialize settings database.
 */
int
settings_init()
{
	struct stat st;
	pthread_mutexattr_t lockattr;

	DEBUG_PRINT("settings", "Initializing settings database");

	/* initialize mutex */
	pthread_mutexattr_init(&lockattr);
	pthread_mutexattr_settype(&lockattr, PTHREAD_MUTEX_RECURSIVE);
	if (pthread_mutex_init(&dblock, &lockattr) != 0) {
		LOG_PRINT_ERROR("Could not initialize mutex!");
	}

	/* check that the database exists and we can
	 * access it. If it's not try to create it */
	while (stat(settings_getdbfile(), &st) == -1 ||
		access(settings_getdbfile(), R_OK|W_OK) == -1) {
		if (settings_createdb() == -1) {
			LOG_VPRINT_ERROR("Could not create database: %s (%d)",
				strerror(errno), errno);
			return -1;
		}
	}

	return 0;
}


/**
 * Shutdown settings database.
 */
void
settings_shutdown()
{
}
