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


#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>


/**
 * write_or_die() -- Like write but it guarantees that it will
 * write the requested amount of data and will crash the program
 * on any error condition, including EOF
 */
void
write_or_die(int fd, const void *buf, size_t len)
{
	ssize_t ret;
	size_t written = 0;
	while ((ret = write(fd, ((uint8_t*) buf) + written, len)) != len) {
		if (ret == 0) {
			fprintf(stderr, "write_or_die: EOF!\n");
			abort();

		} else if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "write_or_die: "
				"write() returned %zd (errno=%i,len=%zu,written=%zu)\n",
				ret, errno, len, written);
			abort();
		}
		len -= ret;
		written += ret;
	}
}


/**
 * read_or_die() -- Like read() but it guarantees that it will
 * return the requested amount of data and will crash the program
 * on any error condition, including EOF.
 */
void
read_or_die(int fd, void *buf, size_t length)
{
	ssize_t ret;
	size_t bytes_read = 0;
	while ((ret = read(fd, ((uint8_t*) buf) + bytes_read, length)) != length) {
		if (ret == 0) {
			fprintf(stderr, "read_or_die: EOF!\n");
			abort();

		} else if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "read_or_die: "
				"read() returned %zd (errno=%i,length=%zu,bytes_read=%zu)\n",
				ret, errno, length, bytes_read);
			abort();
		}
		bytes_read += ret;
		length -= ret;
	}
}

/**
 * read_or_eof() -- Like read() but it will either successfuly read
 * the amount requested, return EOF, or crash the program on any other
 * error condition
 */
int
read_or_eof(int fd, void *buf, size_t length)
{
	ssize_t ret;
	size_t bytes_read = 0;
	while ((ret = read(fd, ((uint8_t*) buf) + bytes_read, length)) != length) {
		if (ret == 0) {
			/* EOF after some bytes read should never happen */
			if (bytes_read != 0) {
				fprintf(stderr, "read_or_eof(): EOF after %zd bytes read.\n",
					bytes_read);
				abort();
			}
			return 0; /* eof */
		} else if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "read_or_die: "
				"read() returned %zd (errno=%i,length=%zu,bytes_read=%zu)\n",
				ret, errno, length, bytes_read);
			abort();
		}
		bytes_read += ret;
		length -= ret;
	}
	return (bytes_read + length);
}

int
write_or_epipe(int fd, void *buf, size_t size)
{
	ssize_t ret;
	size_t bytes_written = 0;
	while ((ret = write(fd, ((uint8_t*) buf) + bytes_written, size)) != size) {
		if (ret == -1) {
			if (errno == EINTR) {
				continue;
			} else if (errno == EPIPE) {
				return 0;
			}
		} else if (ret == 0) {
			fprintf(stderr, "write_or_epipe() aborted!\n");
			abort();
		}
		bytes_written += ret;
		size -= ret;
	}
	return (bytes_written + size);
}

