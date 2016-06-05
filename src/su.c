#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>

static uid_t mb_uid = 0;
static gid_t mb_gid = 0;


static void
get_mediabox_user(uid_t *uid, gid_t *gid)
{
	struct passwd *pw;
	errno = 0;
	pw = getpwnam("mediabox");
	if (pw == NULL) {
		fprintf(stderr, "mb: mediabox user not found. errno=%i\n",
			errno);
		*uid = *gid = 0;
		return;
	}
	*uid = pw->pw_uid;
	*gid = pw->pw_gid;
}


int
mb_su_canroot(void)
{
	return (getuid() == 0);
}


int
mb_su_gainroot(void)
{
	if (geteuid() == 0) {
		return 0; /* already root */

	} else 	if (getuid() != 0) {
		return -1; /* never root */

	} else if (seteuid(0) == -1) {
		return -1;

	}
	(void) setegid(0);
	return 0;
}


void
mb_su_droproot(void)
{
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

