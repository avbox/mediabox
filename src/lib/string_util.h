/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2018 Fernando Rodriguez
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


#ifndef __STRING_UTIL_H__
#define __STRING_UTIL_H__


/**
 * StringBuilder object.
 */
struct avbox_stringbuilder;


/**
 * Gets a copy of the underlying C string.
 */
char*
avbox_stringbuilder_strdup(const struct avbox_stringbuilder*const inst);


/**
 * Get the length of the internally stored string.
 */
size_t
avbox_stringbuilder_size(const struct avbox_stringbuilder*const inst);


/**
 * Get the internally stored string.
 */
const char*
avbox_stringbuilder_cstr(const struct avbox_stringbuilder*const inst);


/**
 * Append a C string to the stringbuilder object.
 */
ssize_t
avbox_stringbuilder_append(struct avbox_stringbuilder*const inst,
	const char*const str);

/**
 * Destroy a stringbuilder object.
 */
void
avbox_stringbuilder_destroy(struct avbox_stringbuilder*const inst);


/**
 * Create a new stringbuilder object.
 */
struct avbox_stringbuilder*
avbox_stringbuilder_new(int capacity);


/**
 * Like isdigit() but works on strings.
 */
int
strisdigit(const char *str);


/**
 * Takes a pointer to a malloc allocated string and returns
 * a pointer to a malloc allocated string with the
 * replacements. If the replacements cannot be done in place
 * a new buffer will be allocated for the string and str
 * will be freed.
 */
char *
strreplace(char *str, const char * const what, const char * const with);


/**
 * Trims the string in place.
 */
char *
strtrim(char * const str);


/**
 * Return 1 if str1 ends with str2. Zero otherwise.
 */
int
strendswith(const char * const str1, const char * const str2);


#endif
