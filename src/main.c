#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "video.h"
#include "input.h"
#include "player.h"

#define MEDIA_FILEi "/media/InternalStorage/Movies/Avatar ECE (2009)/Avatar.ECE.2009.720p.BrRip.x264.bitloks.YIFY.mp4"
#define MEDIA_FILE "/mnt/shared/movies/The Shawshank Redemption (1994)/The Shawshank Redemption 1994.720p.BRRip.x264.YIFY.mp4"

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
	//(void) mbp_play(player, MEDIA_FILE);

	/* test window drawing code */
	struct mbv_window *win;
	win = mbv_window_new("Hello World",
		(mbv_screen_width_get() / 2) - 150,
		(mbv_screen_height_get() / 2) - 150,
		300,
		300);
	mbv_window_show(win);
	sleep(3);
	mbv_window_hide(win);
	sleep(3);
	mbv_window_show(win);
	sleep(3);
	mbv_destroy(win);

	/* run the input loop */
	ret = mbi_loop(input);

	/* destroy the media player */
	mbp_destroy(player);
	mbi_destroy(input);
	mbv_destroy();

	return ret;
}

