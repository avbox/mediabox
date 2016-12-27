#ifndef __MB_LIBRARY_BACKEND_H__
#define __MB_LIBRARY_BACKEND_H__


/**
 * mb_library_backend_init() -- Initialize the library backend.
 */
int
mb_library_backend_init(const int launch_avmount);


/**
 * mb_library_backend_shutdown() -- Shutdown the library backend.
 */
void
mb_library_backend_shutdown(void);


#endif
