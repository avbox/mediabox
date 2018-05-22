/*
 * stream.h : access to the content of a remote file.
 * This file is part of mediabox.
 *
 * (C) Copyright 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __STREAM_H__
#define __STREAM_H__


#include <stdlib.h>


struct avbox_httpstream;


/**
 * Initialize the cURL library.
 */
void
avbox_httpstream_init(void);


/**
 * Opens a URL for streaming
 */
struct avbox_httpstream*
avbox_httpstream_open(const char *url);


/**
 * Reads data from a network stream
 */
ssize_t
avbox_httpstream_read(struct avbox_httpstream * const file,
	void *ptr, size_t size);


/**
 * Seek the network stream
 */
void
avbox_httpstream_seek(struct avbox_httpstream * const file,
	off_t offset);

/**
 * Closes a URL
 */
void
avbox_httpstream_close(struct avbox_httpstream * const file);


/**
 * Free all memory allocated by all streams.
 * Must be called before exec();
 */
void
avbox_httpstream_shutdown(void);


#endif
