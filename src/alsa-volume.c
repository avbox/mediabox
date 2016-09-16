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
