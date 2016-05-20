#include <stdlib.h>
#include <stdio.h>

#include "video-directfb.h"
#include "input.h"
#include "player.h"

#define MEDIA_FILE "/media/InternalStorage/Movies/Avatar ECE (2009)/Avatar.ECE.2009.720p.BrRip.x264.bitloks.YIFY.mp4"

struct mbi* input = NULL;
struct mbp* player = NULL;

int
main (int argc, char **argv)
{
	int ret;

	/* initialize video device */
	mbv_init(argc, argv);

	/* initialize input driver */
	if ((input = mbi_init()) == NULL) {
		fprintf(stderr, "Could not initialize input device(s)\n");
		exit(EXIT_SUCCESS);
	}

	/* initialize the media player */
	if ((player = mbp_init(input)) == NULL) {
		fprintf(stderr, "Could not initialize player\n");
		exit(EXIT_FAILURE);
	}

	/* play video file */
	(void) mbp_play(player, MEDIA_FILE);

	/* run the input loop */
	ret = mbi_loop(input);

	/* destroy the media player */
	mbp_destroy(player);
	mbi_destroy(input);
	mbv_destroy();

	return ret;
}

