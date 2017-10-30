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


#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#define LOG_MODULE "su"

#include "log.h"


static uid_t mb_uid = 0;
static gid_t mb_gid = 0;


static void
get_mediabox_user(uid_t *uid, gid_t *gid)
{
	struct passwd *pw;
	errno = 0;
	pw = getpwnam("mediabox");
	if (pw == NULL) {
		LOG_PRINT_WARN("User mediabox not found!");
		*uid = *gid = 0;
		return;
	}
	*uid = pw->pw_uid;
	*gid = pw->pw_gid;
}


int
avbox_canroot(void)
{
	return (getuid() == 0);
}


int
avbox_gainroot(void)
{
	if (geteuid() == 0) {
		/* we're already root */
		return 0;

	} else 	if (getuid() != 0) {
		/* the program was not started as root so we can't get root */
		LOG_PRINT_ERROR("Cannot gain root access: Not started as root!");
		return -1;

	} else if (seteuid(0) == -1) {
		LOG_VPRINT_ERROR("Could not set effective user id: %s",
			strerror(errno));
		return -1;
	}
	if (setegid(0) != 0) {
		LOG_VPRINT_ERROR("Could not set effective group id: %s",
			strerror(errno));
	}
	return 0;
}


void
avbox_droproot(void)
{
	get_mediabox_user(&mb_uid, &mb_gid);

	return;
	#if 0
	if (getuid() != 0 && geteuid() != 0) {
		return; /* program running as regular user */
	}

	/* drop root prividges after initializing framebuffer */
	if (getuid() == 0) {
		if (mb_uid == 0 && mb_gid == 0) {
			get_mediabox_user(&mb_uid, &mb_gid);
		}
		if (mb_uid && mb_gid) {
			fprintf(stderr, "mb: Droping root priviledge\n");
			if (setegid(mb_gid) == -1) {
				fprintf(stderr, "mb: WARNING!! "
					"setgid() failed. Runing as group root.\n");
			}
			if (seteuid(mb_uid) == -1) {
				fprintf(stderr, "mb: WARNING!! "
					"setuid() failed. Running as root.\n");
			}
		} else {
			fprintf(stderr, "mb: WARNING!! "
				"mediabox user not found! Running as root.\n");
		}
		return;
	}

	fprintf(stderr, "mediabox: Could not drop root rights. Aborting.\n");

	abort();
	#endif
}

