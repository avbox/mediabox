/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __PROCESS_H__
#define __PROCESS_H__


/* Process flags enumerator */
enum mb_process_flags
{
	MB_PROCESS_NONE			= 0x00000000,
	MB_PROCESS_AUTORESTART		= 0x00000001,
	MB_PROCESS_SIGKILL		= 0x00000002,
	MB_PROCESS_SUPERUSER		= 0x00000004,
	MB_PROCESS_NICE			= 0x00000008,
	MB_PROCESS_IONICE_IDLE		= 0x00000010,
	MB_PROCESS_IONICE_BE		= 0x00000020,
	MB_PROCESS_IONICE_RT		= 0x00000040,
	MB_PROCESS_STDOUT_LOG		= 0x00000080,
	MB_PROCESS_STDOUT_PIPE		= 0x00000100,
	MB_PROCESS_STDERR_LOG		= 0x00000200,
	MB_PROCESS_STDERR_PIPE		= 0x00000400,
	MB_PROCESS_WAIT			= 0x00000800,

	MB_PROCESS_IONICE = (MB_PROCESS_IONICE_IDLE | MB_PROCESS_IONICE_BE | MB_PROCESS_IONICE_RT),
	MB_PROCESS_STDOUT = (MB_PROCESS_STDOUT_LOG | MB_PROCESS_STDOUT_PIPE),
	MB_PROCESS_STDERR = (MB_PROCESS_STDERR_LOG | MB_PROCESS_STDERR_PIPE)
};


/* Process exit callback function */
typedef void (*mb_process_exit)(int id, int exit_status,
	void *data);


/**
 * mb_process_wait() -- Wait for a process to exit.
 */
int
mb_process_wait(int id, int *exit_status);


/**
 * mb_process_openfd() -- Opens one of the standard file descriptors for
 * the process.
 *
 * NOTE: After opening a file descriptor with this function the process
 * manager stops managing the file descriptor so you must call close()
 * on it when you're done using it.
 */
int
mb_process_openfd(int id, int std_fileno);


/**
 * Set the amount of time, in seconds, to wait for a process
 * to exit after sending SIGTERM before sending SIGKILL.
 */
int
mb_process_setsigkilldelay(int procid, unsigned delay);


int
mb_process_start(const char *binary, const char * const argv[], enum mb_process_flags flags,
	const char *name, mb_process_exit exit_callback, void *exit_callback_data);


int
mb_process_stop(int id);


int
mb_process_init(void);


void
mb_process_shutdown(void);

#endif
