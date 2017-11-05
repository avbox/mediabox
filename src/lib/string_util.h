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


#ifndef __STRING_UTIL_H__
#define __STRING_UTIL_H__


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

#endif
