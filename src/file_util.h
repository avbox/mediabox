#ifndef __FILE_UTIL_H__
#define __FILE_UTIL_H__


/**
 * closefrom() -- Close all file descriptors >= fd_max.
 */
int
closefrom(int fd_max);

#endif
