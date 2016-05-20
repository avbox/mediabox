#include <stdlib.h>
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
	while ((ret = write(fd, buf + written, len)) != len) {
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
	while ((ret = read(fd, buf + bytes_read, length)) != length) {
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
	while ((ret = read(fd, buf + bytes_read, length)) != length) {
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

