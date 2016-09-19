#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <alsa/mixer.h>

#include "debug.h"
#include "log.h"
#include "alsa-volume.h"


#define AMIXER_BIN "/usr/bin/amixer"


static mb_alsa_volumechanged volume_callback = NULL;
static const char *card = "default";
static const char *selem_name = "Master";


int
mb_alsa_volume_get(void)
{
	int err, ret = -1;
	long min, max, volume;
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t* elem;

	if ((err = snd_mixer_open(&handle, 0)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "snd_mixer_open() failed: %s",
			snd_strerror(err));
		return -1;
	}
	if ((err = snd_mixer_attach(handle, card)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "snd_mixer_attach() failed: %s",
			snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_selem_register(handle, NULL, NULL)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "snd_mixer_selem_register() failed %s",
			snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_load(handle)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "snd_mixer_load() failed: %s",
			snd_strerror(err));
		goto end;
	}

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, selem_name);

	if ((elem = snd_mixer_find_selem(handle, sid)) == NULL) {
		LOG_PRINT_ERROR("soundctl", "snd_mixer_find_selem() returned NULL");
		goto end;
	}
	if ((err = snd_mixer_selem_get_playback_volume_range(elem, &min, &max)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "snd_mixer_selem_get_playback_volume_range() failed: %s",
			snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_UNKNOWN, &volume)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "snd_mixer_selem_get_playback_volume() failed: %s",
			snd_strerror(ret));
		goto end;
	}

	ret = (int) ((volume * 100) / max);

end:
	snd_mixer_close(handle);

	return ret;
}


int
mb_alsa_volume_set(int volume)
{
	int err, ret = -1;
	long min, max;
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t* elem;

	if ((err = snd_mixer_open(&handle, 0)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "Could not open mixer: %s",
			snd_strerror(err));
		return -1;
	}
	if ((err = snd_mixer_attach(handle, card)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "Could not attach mixer: %s",
			snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_selem_register(handle, NULL, NULL)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "Could not register: %s",
			snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_load(handle)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "Could not load mixer: %s",
			snd_strerror(err));
		goto end;
	}

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, selem_name);

	if ((elem = snd_mixer_find_selem(handle, sid)) == NULL) {
		LOG_PRINT_ERROR("soundctl", "snd_mixer_find_selem() returned NULL");
		goto end;
	}
	if ((err = snd_mixer_selem_get_playback_volume_range(elem, &min, &max)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "Could not get volume range: %s",
			snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100)) < 0) {
		LOG_VPRINT_ERROR("soundctl", "Could not set volume: %s",
			snd_strerror(err));
		goto end;
	}

	if (volume_callback != NULL) {
		volume_callback(volume);
	}

	ret = 0;

end:
	snd_mixer_close(handle);

	return ret;
}


int
mb_alsa_volume_init(mb_alsa_volumechanged callback)
{
	/* assert(callback != NULL); */
	assert(volume_callback == NULL);
	volume_callback = callback;
	return 0;
}
