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
#include <string.h>
#include <curl/curl.h>
#include <ctype.h>

#define LOG_MODULE "url-util"

#include <libavbox/avbox.h>


struct MemoryStruct
{
	char *memory;
	size_t size;
	size_t limit;
};


/**
 * urldecode() -- Decode url encoded string.
 *
 * Borrowed from ThomasH:
 * https://stackoverflow.com/questions/2673207/c-c-url-decode-library
 */
void
urldecode(char *dst, const char *src)
{
	char a, b;
	while (*src) {
		if ((*src == '%') &&
			((a = src[1]) && (b = src[2])) &&
			(isxdigit(a) && isxdigit(b))) {
			if (a >= 'a')
				a -= 'a'-'A';
			if (a >= 'A')
				a -= ('A' - 10);
			else
				a -= '0';
			if (b >= 'a')
				b -= 'a'-'A';
			if (b >= 'A')
				b -= ('A' - 10);
			else
				b -= '0';
			*dst++ = 16*a+b;
			src+=3;
		} else if (*src == '+') {
			*dst++ = ' ';
			src++;
		} else {
			*dst++ = *src++;
		}
	}
	*dst++ = '\0';
}


static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct MemoryStruct *mem = (struct MemoryStruct *)userp;

	mem->memory = realloc(mem->memory, mem->size + realsize + 1);
	if(mem->memory == NULL) {
		/* out of memory! */
		fprintf(stderr, "not enough memory (realloc returned NULL)\n");
		return 0;
	}

	memcpy(&(mem->memory[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;

	return realsize;
}


/**
 * Download the contents of a given URL to memory synchronously and
 * in one shot. Then save the malloc'd buffer pointer to dest and it's
 * size to *size. If *size >= 0 then it will be used as a size limit.
 * Otherwise this function will block until the entire file is
 * downloaded.
 */
int
avbox_net_geturl(const char * const url, void **dest, size_t *size)
{
	CURL *curl_handle;
	CURLcode res;
	int ret = -1;

	struct MemoryStruct chunk;

	chunk.limit = (*size > 0) ? *size : 0;
	chunk.memory = malloc(1);
	chunk.size = 0;
	if (chunk.memory == NULL) {
		return -1;
	}


	curl_global_init(CURL_GLOBAL_ALL);

	/* init the curl session */
	if ((curl_handle = curl_easy_init()) == NULL) {
		LOG_PRINT_ERROR("curl_easy_init() failed");
		goto end;
	}

	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
	curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "AVBoX/" PACKAGE_VERSION);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, FALSE);
	//curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64; rv:46.0) Gecko/20100101 Firefox/46.0");

	/* get it! */
	res = curl_easy_perform(curl_handle);

	/* check for errors */
	if(res != CURLE_OK) {
		LOG_VPRINT_ERROR("curl_easy_perform failed: %s",
			curl_easy_strerror(res));
		goto end;

	} else {
		*dest = chunk.memory;
		*size = chunk.size;
		ret = 0;
	}

end:

	if (ret == -1) {
		free(chunk.memory);
	}

	curl_easy_cleanup(curl_handle);
	curl_global_cleanup();
	return ret;
}

