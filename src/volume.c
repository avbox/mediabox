/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <alsa/mixer.h>

#define LOG_MODULE "volume"

#include "debug.h"
#include "log.h"
#include "volume.h"
#include "input.h"
#include "settings.h"


static int message_fd = -1;
static const char *card = "default";
static const char *selem_name = "Master";


int
avbox_volume_get(void)
{
	int err, ret = -1;
	long min, max, volume;
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t* elem;

	DEBUG_PRINT("volume", "avbox_volume_get()");

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
	if ((err = snd_mixer_selem_get_playback_volume(elem,
		SND_MIXER_SCHN_FRONT_LEFT, &volume)) < 0) {
		LOG_VPRINT_ERROR("snd_mixer_selem_get_playback_volume() failed: %s",
			snd_strerror(ret));
		goto end;
	}

	DEBUG_VPRINT("volume", "min=%d max=%d vol=%d, percent=%d",
		min, max, volume, (volume * 100) / max);

	ret = (int) ((volume * 100) / max);

end:
	snd_mixer_close(handle);

	return ret;
}


int
avbox_volume_set(int volume)
{
	int err, ret = -1;
	long min, max;
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t* elem;

	DEBUG_VPRINT("volume", "Setting volume to %d",
		volume);

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
		avbox_input_sendmessage(message_fd, MBI_EVENT_VOLUME_CHANGED, &volume,
			sizeof(volume));
	}

	/* save the volume */
	avbox_settings_setint("volume", volume);
	ret = 0;

end:
	snd_mixer_close(handle);

	return ret;
}


int
avbox_volume_init(int msgfd)
{
	assert(message_fd == -1);

	/* set the volume to either the last known
	 * volume or a default value of 60 */
	avbox_volume_set(avbox_settings_getint("volume", 60));

	message_fd = msgfd;

	return 0;
}


void
avbox_volume_shutdown(void)
{
	message_fd = -1;
}
