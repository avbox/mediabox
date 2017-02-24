#ifndef __MB_LIBRARY_BACKEND_H__
#define __MB_LIBRARY_BACKEND_H__


/**
 * Initialize the library backend.
 */
int
mb_library_backend_init(const int launch_avmount,
	const int launch_mediatomb);


/**
 * Shutdown the library backend.
 */
void
mb_library_backend_shutdown(void);


#endif
