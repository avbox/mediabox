#ifndef __PROCESS_H__
#define __PROCESS_H__


/* Process flags enumerator */
enum mb_process_flags
{
	MB_PROCESS_NONE = 0,
	MB_PROCESS_AUTORESTART = 1,
	MB_PROCESS_SIGKILL = 2,
	MB_PROCESS_SUPERUSER = 4,
	MB_PROCESS_NICE = 8,
	MB_PROCESS_IONICE_IDLE = 16,
	MB_PROCESS_IONICE_BE = 32,
	MB_PROCESS_IONICE = (MB_PROCESS_IONICE_IDLE | MB_PROCESS_IONICE_BE)
};


/* Process exit callback function */
typedef int (*mb_process_exit)(int id, int exit_status);


int
mb_process_start(const char *binary, char * const argv[], enum mb_process_flags flags,
	const char *name, mb_process_exit exit_callback);


int
mb_process_stop(int id);


int
mb_process_init(void);


void
mb_process_shutdown(void);

#endif
