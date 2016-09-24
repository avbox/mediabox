#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <alsa/mixer.h>

#define LOG_MODULE "soundctl"

#include "debug.h"
#include "log.h"
#include "alsa-volume.h"
#include "input.h"


static int message_fd = -1;
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
		LOG_VPRINT_ERROR("snd_mixer_open() failed: %s", snd_strerror(err));
		return -1;
	}
	if ((err = snd_mixer_attach(handle, card)) < 0) {
		LOG_VPRINT_ERROR("snd_mixer_attach() failed: %s", snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_selem_register(handle, NULL, NULL)) < 0) {
		LOG_VPRINT_ERROR("snd_mixer_selem_register() failed %s", snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_load(handle)) < 0) {
		LOG_VPRINT_ERROR("snd_mixer_load() failed: %s", snd_strerror(err));
		goto end;
	}

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, selem_name);

	if ((elem = snd_mixer_find_selem(handle, sid)) == NULL) {
		LOG_PRINT_ERROR("snd_mixer_find_selem() returned NULL");
		goto end;
	}
	if ((err = snd_mixer_selem_get_playback_volume_range(elem, &min, &max)) < 0) {
		LOG_VPRINT_ERROR("snd_mixer_selem_get_playback_volume_range() failed: %s",
			snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_UNKNOWN, &volume)) < 0) {
		LOG_VPRINT_ERROR("snd_mixer_selem_get_playback_volume() failed: %s",
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
		LOG_VPRINT_ERROR("Could not open mixer: %s", snd_strerror(err));
		return -1;
	}
	if ((err = snd_mixer_attach(handle, card)) < 0) {
		LOG_VPRINT_ERROR("Could not attach mixer: %s", snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_selem_register(handle, NULL, NULL)) < 0) {
		LOG_VPRINT_ERROR("Could not register: %s", snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_load(handle)) < 0) {
		LOG_VPRINT_ERROR("Could not load mixer: %s", snd_strerror(err));
		goto end;
	}

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, selem_name);

	if ((elem = snd_mixer_find_selem(handle, sid)) == NULL) {
		LOG_PRINT_ERROR("snd_mixer_find_selem() returned NULL");
		goto end;
	}
	if ((err = snd_mixer_selem_get_playback_volume_range(elem, &min, &max)) < 0) {
		LOG_VPRINT_ERROR("Could not get volume range: %s", snd_strerror(err));
		goto end;
	}
	if ((err = snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100)) < 0) {
		LOG_VPRINT_ERROR("Could not set volume: %s", snd_strerror(err));
		goto end;
	}

	if (message_fd != -1) {
		mbi_sendmessage(message_fd, MBI_EVENT_VOLUME_CHANGED, &volume,
			sizeof(volume));
	}

	ret = 0;

end:
	snd_mixer_close(handle);

	return ret;
}


int
mb_alsa_volume_init(int msgfd)
{
	assert(message_fd == -1);

	message_fd = msgfd;

	return 0;
}


void
mb_alsa_volume_destroy(void)
{
	message_fd = -1;
}
