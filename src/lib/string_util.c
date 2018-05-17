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

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define LOG_MODULE "string_util"

#include "debug.h"
#include "log.h"

#define AVBOX_STRINGBUILDER_EXTRA_ROOM	(512)


struct avbox_stringbuilder
{
	char* buf;
	size_t size;
	size_t capacity;
};


char*
avbox_stringbuilder_strdup(const struct avbox_stringbuilder*const inst)
{
	return strdup(inst->buf);
}

size_t
avbox_stringbuilder_size(const struct avbox_stringbuilder*const inst)
{
	return inst->size;
}


const char*
avbox_stringbuilder_cstr(const struct avbox_stringbuilder*const inst)
{
	return inst->buf;
}


ssize_t
avbox_stringbuilder_append(struct avbox_stringbuilder*const inst,
	const char*const str)
{
	ASSERT(inst != NULL);
	ASSERT(inst->buf != NULL);
	ASSERT(str != NULL);

	const size_t len = strlen(str);

	/* grow the buffer if needed */
	if (inst->capacity <= (inst->size + len)) {
		const int capacity = inst->size + len +
			AVBOX_STRINGBUILDER_EXTRA_ROOM;
		char*const buf = malloc(capacity);
		if (buf == NULL) {
			ASSERT(errno == ENOMEM);
			return -1;
		}
		memcpy(buf, inst->buf, inst->size + 1);
		free(inst->buf);
		inst->buf = buf;
		inst->capacity = capacity;
	}

	/* append the string, including the null terminator */
	memcpy(inst->buf + inst->size, str, len + 1);
	inst->size += len;
	return inst->size;
}


void
avbox_stringbuilder_destroy(struct avbox_stringbuilder*const inst)
{
	ASSERT(inst != NULL);
	ASSERT(inst->buf != NULL);
	free(inst->buf);
	free(inst);
}


struct avbox_stringbuilder*
avbox_stringbuilder_new(int capacity)
{
	struct avbox_stringbuilder*const inst =
		malloc(sizeof(struct avbox_stringbuilder));
	if (inst == NULL) {
		ASSERT(errno == ENOMEM);
		return NULL;
	}

	if (capacity == 0) {
		capacity = AVBOX_STRINGBUILDER_EXTRA_ROOM;
	}

	if ((inst->buf = malloc(capacity)) == NULL) {
		ASSERT(errno == ENOMEM);
		free(inst);
		return NULL;
	}

	inst->capacity = capacity;
	inst->size = 0;
	inst->buf[0] = '\0';
	return inst;
}


/**
 * strisdigit() -- Like isdigit() but works on strings.
 */
int
strisdigit(const char *str)
{
	while (*str != '\0') {
		if (!isdigit(*str++)) {
			return 0;
		}
	}
	return 1;
}


/**
 * Takes a pointer to a malloc allocated string and returns
 * a pointer to a malloc allocated string with the
 * replacements. If the replacements cannot be done in place
 * a new buffer will be allocated for the string and str
 * will be freed.
 */
char *
strreplace(char *str, const char * const what, const char * const with)
{
	int len, diff;
	char *ins, *ret = NULL;

	ASSERT(str != NULL);
	ASSERT(what != NULL);
	ASSERT(with != NULL);

	const int what_len = strlen(what);
	const int with_len = strlen(with);

	if (strstr(str, what) == NULL) {
		return str;
	}

	/* calculate the length if the new string */
	len = strlen(str);
	diff = with_len - what_len;
	for (ins = strstr(str, what); ins != NULL;
		len += diff, ins = strstr(ins + what_len, what));

	/* allocate memory for new string */
	if ((ret = malloc(len + 1)) == NULL) {
		ASSERT(errno == ENOMEM);
		goto end;
	}

	for (*ret = '\0', ins = str; ins != NULL; ins = ins) {
		char *next = strstr(ins, what);
		if (next == NULL) {
			strcat(ret, ins);
			ins = NULL;
		} else {
			int sublen = next - ins;
			strncat(ret, ins, sublen);
			strcat(ret, with);
			ins += sublen + what_len;
		}
	}
end:
	free(str);
	return ret;
}


/**
 * Trims the string in place.
 */
char *
strtrim(char * const str)
{
	char *pstr;
	size_t len;

	if (str == NULL) {
		errno = EINVAL;
		return NULL;
	}


	/* trim spaces of the end */
	len = strlen(str);
	pstr = str + len - 1;
	while (len > 0 && (*pstr == ' ' || *pstr == '\t')) {
		*pstr-- = '\0';
		len--;
	}

	/* trim spaces of the start */
	pstr = str;
	while (*pstr != '\0' && (*pstr == ' ' || *pstr == '\t')) {
		pstr++;
		len--;
	}

	ASSERT(len >= 0);

	if (pstr > str) {
		memmove(str, pstr, len + 1);
	}

	return str;
}


/**
 * Return 1 if str1 ends with str2. Zero otherwise.
 */
int
strendswith(const char * const str1, const char * const str2)
{
	const size_t len1 = strlen(str1);
	const size_t len2 = strlen(str2);
	if (len1 < len2) {
		return 0;
	}
	const int ret = !strcmp(str1 + (len1 - len2), str2) ? 1 : 0;
	DEBUG_VPRINT(LOG_MODULE, "RETURNING %i", ret);
	return ret;
}
