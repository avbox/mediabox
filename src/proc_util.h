#ifndef __MB_PROC_UTIL_H__
#define __MB_PROC_UTIL_H__


/**
 * Gets the path of the current process' executable image.
 */
ssize_t
mb_getexepath(char *buf, size_t bufsize);


#endif
