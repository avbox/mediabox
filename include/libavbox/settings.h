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
