#ifndef __AVBOX_DBUTIL__
#define __AVBOX_DBUTIL__


/**
 * Request a filename in the writable partiton.
 */
char *
avbox_dbutil_getdbfile(const char * const filename);


/**
 * Escape sql string. Returns a new copy that must
 * be freed.
 */
char *
avbox_dbutil_escapesql(const char * const sql);

#endif
