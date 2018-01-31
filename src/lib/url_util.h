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


#ifndef __MB_URL_UTIL_H__
#define __MB_URL_UTIL_H__


void
urldecode(char *dst, const char *src);


/**
 * Download the contents of a given URL to memory synchronously and
 * in one shot. Then save the malloc'd buffer pointer to dest and it's
 * size to *size. If *size >= 0 then it will be used as a size limit.
 * Otherwise this function will block until the entire file is
 * downloaded.
 */
int
avbox_net_geturl(const char * const url, void **dest, size_t *size);


#endif
