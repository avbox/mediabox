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

#define MEDIA_FILEi "/media/InternalStorage/Movies/Avatar ECE (2009)/Avatar.ECE.2009.720p.BrRip.x264.bitloks.YIFY.mp4"
#define MEDIA_FILE "/mnt/shared/movies/The Shawshank Redemption (1994)/The Shawshank Redemption 1994.720p.BRRip.x264.YIFY.mp4"

struct mbp* player = NULL;


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
	uid_t mb_uid;
	gid_t mb_gid;

	/* initialize video device */
	mbv_init(argc, argv);

	/* initialize input system */
	if (mbi_init() != 0) {
		fprintf(stderr, "Could not initialize input device(s)\n");
		exit(EXIT_FAILURE);
	}

	/* drop root prividges after initializing framebuffer */
	if (getuid() == 0) {
		get_mediabox_user(&mb_uid, &mb_gid);
		if (mb_uid && mb_gid) {
			fprintf(stderr, "mb: Droping root priviledge\n");
			if (setgid(mb_gid) == -1) {
				fprintf(stderr, "mb: WARNING!! "
					"setgid() failed. Runing as group root.\n");
			}
			if (setuid(mb_uid) == -1) {
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

	/* initialize the media player */
	//if ((player = mbp_init()) == NULL) {
	//	fprintf(stderr, "Could not initialize player\n");
	//	exit(EXIT_FAILURE);
	//}

	/* play video file */
	//(void) mbp_play(player, MEDIA_FILE);

	/* destroy the media player */
	//mbp_destroy(player);
	mbs_destroy();
	mbi_destroy();
	mbv_destroy();

	return 0;
}

