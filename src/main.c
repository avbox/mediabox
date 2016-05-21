#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "video.h"
#include "input.h"
#include "player.h"
#include "shell.h"

#define MEDIA_FILEi "/media/InternalStorage/Movies/Avatar ECE (2009)/Avatar.ECE.2009.720p.BrRip.x264.bitloks.YIFY.mp4"
#define MEDIA_FILE "/mnt/shared/movies/The Shawshank Redemption (1994)/The Shawshank Redemption 1994.720p.BRRip.x264.YIFY.mp4"

struct mbp* player = NULL;

int
main (int argc, char **argv)
{
	/* initialize video device */
	mbv_init(argc, argv);

	/* initialize input system */
	if (mbi_init() != 0) {
		fprintf(stderr, "Could not initialize input device(s)\n");
		exit(EXIT_SUCCESS);
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

