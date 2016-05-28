#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>

#include "video.h"
#include "input.h"
#include "player.h"
#include "shell.h"

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
main (int argc, char **argv)
{
	/* initialize video device */
	mbv_init(argc, argv);

	/* initialize input system */
	if (mbi_init() != 0) {
		fprintf(stderr, "Could not initialize input device(s)\n");
		exit(EXIT_FAILURE);
	}

	/* drop root prividges after initializing framebuffer */
	if (0 && getuid() == 0) {
		uid_t mb_uid;
		gid_t mb_gid;

		get_mediabox_user(&mb_uid, &mb_gid);
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
	}

	/* initialize the shell */
	if (mbs_init() != 0) {
		fprintf(stderr, "Could not initialize shell\n");
		exit(EXIT_FAILURE);
	}

	/* show the shell */
	mbs_show_dialog();

	/* cleanup */
	mbs_destroy();
	mbi_destroy();
	mbv_destroy();

	return 0;
}

