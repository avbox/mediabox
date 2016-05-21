#ifndef __PIPE_UTIL_H__
#define __PIPE_UTIL_H__

/**
 * write_or_die() -- Like write but it guarantees that it will
 * write the requested amount of data and will crash the program
 * on any error condition, including EOF
 */
void
write_or_die(int fd, const void *buf, size_t len);

int
write_or_epipe(int fd, void *buf, size_t size);

/**
 * read_or_die() -- Like read() but it guarantees that it will
 * return the requested amount of data and will crash the program
 * on any error condition, including EOF.
 */
void
read_or_die(int fd, void *buf, size_t length);

/**
 * read_or_eof() -- Like read() but it will either successfuly read
 * the amount requested, return EOF, or crash the program on any other
 * error condition
 */
int
read_or_eof(int fd, void *buf, size_t length);

#endif

