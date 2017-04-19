#ifndef __MB_SETTINGS_H__
#define __MB_SETTINGS_H__


/**
 * Gets the value of a setting. The result must
 * be freed with free().
 */
char *
settings_getstring(const char * const key);


/**
 * Sets a string value on the settings database.
 */
int
settings_setstring(const char * const key,
	const char * const value);


/**
 * Gets a boolean value from the settings database.
 */
int
settings_getbool(const char * const key);


/**
 * Sets a boolean value on the settings database.
 */
int
settings_setbool(const char * const key, const int value);


int
settings_init();


/**
 * Shutdown settings database.
 */
void
settings_shutdown();

#endif
