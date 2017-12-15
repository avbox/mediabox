/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include <stdlib.h>
#include <stdint.h>
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
#include "dispatch.h"


static struct avbox_object *msgobj;
static const char *card = "default";
static const char *selem_name = "PCM";
static const int selem_index = 0;
static snd_mixer_t *handle;
static snd_mixer_elem_t* elem;
static long min, max, volume;


int
avbox_volume_get(void)
{
	int err;

	DEBUG_PRINT("volume", "avbox_volume_get()");

	if (elem == NULL) {
		errno = ENOTSUP;
		return -1;
	}

	if ((err = snd_mixer_selem_get_playback_volume(elem,
		SND_MIXER_SCHN_FRONT_LEFT, &volume)) < 0) {
		LOG_VPRINT_ERROR("snd_mixer_selem_get_playback_volume() failed: %s",
			snd_strerror(err));
		return -1;
	}

	return (int) ((volume * 100) / max);
}


int
avbox_volume_set(int volume)
{
	int err, ret = -1;
	static int vol;

	DEBUG_VPRINT("volume", "Setting volume to %d",
		volume);

	if (elem == NULL) {
		errno = ENOTSUP;
		return -1;
	}

	if ((err = snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100)) < 0) {
		LOG_VPRINT_ERROR("Could not set volume: %s", snd_strerror(err));
		return -1;
	}

	/* if the device has a playback switch try to enable it */
	if (snd_mixer_selem_has_common_switch(elem) ||
		snd_mixer_selem_has_playback_switch(elem)) {
		DEBUG_PRINT("volume", "Setting common switch on");
		if ((err = snd_mixer_selem_set_playback_switch_all(elem, 1)) < 0) {
			LOG_VPRINT_ERROR("Could not set playback switch: %s",
				snd_strerror(err));
		}
	}

	if (msgobj != NULL) {
		vol = volume;
		if (avbox_object_sendmsg(&msgobj, AVBOX_MESSAGETYPE_VOLUME,
			AVBOX_DISPATCH_UNICAST, &vol) == NULL) {
			LOG_VPRINT_ERROR("Could not send volume changed message: %s",
				strerror(errno));
		}
	}

	/* save the volume */
	avbox_settings_setint("volume", volume);

	return ret;
}


int
avbox_volume_init(struct avbox_object *obj)
{
	int err;
	snd_mixer_selem_id_t *sid;

	ASSERT(msgobj == NULL);

	if ((err = snd_mixer_open(&handle, 0)) < 0) {
		LOG_VPRINT_ERROR("Could not open mixer: %s", snd_strerror(err));
		handle = NULL;
		elem = NULL;
		return -1;
	}
	if ((err = snd_mixer_attach(handle, card)) < 0) {
		LOG_VPRINT_ERROR("Could not attach mixer: %s", snd_strerror(err));
		snd_mixer_close(handle);
		handle = NULL;
		elem = NULL;
		return -1;
	}
	if ((err = snd_mixer_selem_register(handle, NULL, NULL)) < 0) {
		LOG_VPRINT_ERROR("Could not register: %s", snd_strerror(err));
		snd_mixer_close(handle);
		handle = NULL;
		elem = NULL;
		return -1;
	}
	if ((err = snd_mixer_load(handle)) < 0) {
		LOG_VPRINT_ERROR("Could not load mixer: %s", snd_strerror(err));
		snd_mixer_close(handle);
		handle = NULL;
		elem = NULL;
		return -1;
	}

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, selem_index);
	snd_mixer_selem_id_set_name(sid, selem_name);

	if ((elem = snd_mixer_find_selem(handle, sid)) == NULL) {
		LOG_PRINT_ERROR("snd_mixer_find_selem() returned NULL");
		snd_mixer_close(handle);
		handle = NULL;
		elem = NULL;
		return -1;
	}
	if ((err = snd_mixer_selem_get_playback_volume_range(elem, &min, &max)) < 0) {
		LOG_VPRINT_ERROR("Could not get volume range: %s", snd_strerror(err));
		snd_mixer_close(handle);
		handle = NULL;
		elem = NULL;
		return -1;
	}

	/* set the volume to either the last known
	 * volume or a default value of 60 */
	avbox_volume_set(avbox_settings_getint("volume", 60));

	msgobj = obj;

	return 0;
}


void
avbox_volume_shutdown(void)
{
	if (handle != NULL) {
		snd_mixer_close(handle);
	}
	msgobj = NULL;
}
