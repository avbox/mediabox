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


#ifndef __PROCESS_H__
#define __PROCESS_H__


/* Process flags enumerator */
enum avbox_process_flags
{
	AVBOX_PROCESS_NONE			= 0x00000000,
	AVBOX_PROCESS_AUTORESTART		= 0x00000001,
	AVBOX_PROCESS_SIGKILL			= 0x00000002,
	AVBOX_PROCESS_SUPERUSER			= 0x00000004,
	AVBOX_PROCESS_NICE			= 0x00000008,
	AVBOX_PROCESS_IONICE_IDLE		= 0x00000010,
	AVBOX_PROCESS_IONICE_BE			= 0x00000020,
	AVBOX_PROCESS_IONICE_RT			= 0x00000040,
	AVBOX_PROCESS_STDOUT_LOG		= 0x00000080,
	AVBOX_PROCESS_STDOUT_PIPE		= 0x00000100,
	AVBOX_PROCESS_STDERR_LOG		= 0x00000200,
	AVBOX_PROCESS_STDERR_PIPE		= 0x00000400,
	AVBOX_PROCESS_WAIT			= 0x00000800,
	AVBOX_PROCESS_AUTORESTART_ALWAYS	= 0x00001000,

	AVBOX_PROCESS_IONICE = (AVBOX_PROCESS_IONICE_IDLE | AVBOX_PROCESS_IONICE_BE | AVBOX_PROCESS_IONICE_RT),
	AVBOX_PROCESS_STDOUT = (AVBOX_PROCESS_STDOUT_LOG | AVBOX_PROCESS_STDOUT_PIPE),
	AVBOX_PROCESS_STDERR = (AVBOX_PROCESS_STDERR_LOG | AVBOX_PROCESS_STDERR_PIPE)
};


/* Process exit callback function */
typedef int (*avbox_process_exit)(int id, int exit_status,
	void *data);


/**
 * Wait for a process to exit.
 */
int
avbox_process_wait(int id, int *exit_status);


/**
 * Opens one of the standard file descriptors for
 * the process.
 *
 * NOTE: After opening a file descriptor with this function the process
 * manager stops managing the file descriptor so you must call close()
 * on it when you're done using it.
 */
int
avbox_process_openfd(int id, int std_fileno);


/**
 * Set the amount of time, in seconds, to wait for a process
 * to exit after sending SIGTERM before sending SIGKILL.
 */
int
avbox_process_setsigkilldelay(int procid, unsigned delay);


int
avbox_process_start(const char *binary, const char * const argv[], enum avbox_process_flags flags,
	const char *name, avbox_process_exit exit_callback, void *exit_callback_data);


int
avbox_process_stop(int id);


int
avbox_process_init(void);


void
avbox_process_shutdown(void);

#endif
