#ifndef __MB_SETTINGS_H__
#define __MB_SETTINGS_H__


/**
 * Gets the value of a setting. The result must
 * be freed with free().
 */
char *
avbox_settings_getstring(const char * const key);


/**
 * Sets a string value on the settings database.
 */
int
avbox_settings_setstring(const char * const key,
	const char * const value);


/**
 * Gets a boolean value from the settings database.
 */
int
avbox_settings_getbool(const char * const key);


/**
 * Sets a boolean value on the settings database.
 */
int
avbox_settings_setbool(const char * const key, const int value);


/**
 * Sets an integer value on the settings database.
 */
int
avbox_settings_setint(const char * const key, const int value);


/**
 * Gets an integer value from the settings database.
 */
int
avbox_settings_getint(const char * key, const int defvalue);


int
avbox_settings_init();


/**
 * Shutdown settings database.
 */
void
avbox_settings_shutdown();

#endif
