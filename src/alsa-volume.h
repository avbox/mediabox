#ifndef __ALSA_VOLUME_H__
#define __ALSA_VOLUME_H__


typedef void (*mb_alsa_volumechanged)(int volume);


int
mb_alsa_volume_get(void);


int
mb_alsa_volume_set(int volume);


int
mb_alsa_volume_init(mb_alsa_volumechanged);


#endif
