#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <alsa/asoundlib.h>
#include <alsa/mixer.h>

#include "debug.h"
#include "alsa-volume.h"


#define AMIXER_BIN "/usr/bin/amixer"


static mb_alsa_volumechanged volume_callback = NULL;


#if 0
static int
mbexec(const char *command, char * const argv[])
{
	pid_t pid;
	int exit_code, ret;

	if ((pid = fork()) == -1) {
		fprintf(stderr, "alsa-volume: fork() failed\n");
		return -1;

	} else if (pid != 0) { /* parent */
		while ((ret = waitpid(pid, &exit_code, 0)) != 0) {
			if (ret == EINTR) {
				continue;
			} else {
				fprintf(stderr, "alsa-volume: waitpid() returned %i\n",
					ret);
			}
		}
		if (WEXITSTATUS(exit_code) != 0) {
			fprintf(stderr, "alsa-volume: amixer exited with code %i\n",
				WEXITSTATUS(exit_code));
			return -1;
		}

		return WEXITSTATUS(exit_code);

	} else { /* child */
		execv(command, argv);
		exit(EXIT_FAILURE);
	}
}
#endif


int
mb_alsa_volume_get(void)
{
	long min, max, volume;
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t* elem;
	const char *card = "default";
	const char *selem_name = "Master";
	int ret;

	if ((ret = snd_mixer_open(&handle, 0)) < 0) {
		fprintf(stderr, "soundctl: snd_mixer_open() failed: %s\n",
			snd_strerror(ret));
		return -1;
	}
	if ((ret = snd_mixer_attach(handle, card)) < 0) {
		fprintf(stderr, "soundctl: snd_mixer_attach() failed: %s\n",
			snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_mixer_selem_register(handle, NULL, NULL)) < 0) {
		fprintf(stderr, "soundctl: snd_mixer_selem_register() failed %s\n",
			snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_mixer_load(handle)) < 0) {
		fprintf(stderr, "soundctl: snd_mixer_load() failed: %s\n",
			snd_strerror(ret));
		goto end;
	}

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, selem_name);

	if ((elem = snd_mixer_find_selem(handle, sid)) == NULL) {
		fprintf(stderr, "soundctl: snd_mixer_find_selem() failed: %s\n",
			snd_strerror(ret));
		goto end;
	}

	if ((ret = snd_mixer_selem_get_playback_volume_range(elem, &min, &max)) < 0) {
		fprintf(stderr, "soundctl: snd_mixer_selem_get_playback_volume_range() failed: %s\n",
			snd_strerror(ret));
		goto end;
	}
	if ((ret = snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_UNKNOWN, &volume)) < 0) {
		fprintf(stderr, "soundctl: snd_mixer_selem_get_playback_volume() failed: %s\n",
			snd_strerror(ret));
		goto end;
	}

	ret = (int) ((volume * 100) / max);

end:
	snd_mixer_close(handle);

	return (ret < 0) ? -1 : ret;
}


int
mb_alsa_volume_set(int volume)
{
#if 0
	char volume_string[5];

	/* TODO: Implement this using alsa lib */

	if (volume < 0 || volume > 100) {
		return -1;
	}

	snprintf(volume_string, 5, "%i%%", volume);

	DEBUG_VPRINT("alsa-volume", "Setting volume to %s",
		volume_string);

	return mbexec(AMIXER_BIN, (char * const[]) {
			strdup("amixer"),
			strdup("set"),
			strdup("Master"),
			strdup(volume_string),
			NULL });
#endif

	long min, max;
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	const char *card = "default";
	const char *selem_name = "Master";

	snd_mixer_open(&handle, 0);
	snd_mixer_attach(handle, card);
	snd_mixer_selem_register(handle, NULL, NULL);
	snd_mixer_load(handle);

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, selem_name);
	snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

	snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
	snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100);

	snd_mixer_close(handle);

	if (volume_callback != NULL) {
		volume_callback(volume);
	}

	return 0;
}


int
mb_alsa_volume_init(mb_alsa_volumechanged callback)
{
	/* assert(callback != NULL); */
	assert(volume_callback == NULL);
	volume_callback = callback;
	return 0;
}
