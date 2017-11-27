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


#ifndef __MB_PLAYER__
#define __MB_PLAYER__

#include "video.h"
#include "input.h"
#include "../linkedlist.h"
#include "../dispatch.h"

#define AVBOX_PLAYER_SEEK_ABSOLUTE	(0x01)
#define AVBOX_PLAYER_SEEK_CHAPTER	(0x02)
#define AVBOX_PLAYER_SEEK_RELATIVE	(0x04)

#define AVBOX_PLAYERCTL_PLAY				(0x01)
#define AVBOX_PLAYERCTL_PAUSE				(0x02)
#define AVBOX_PLAYERCTL_STOP				(0x03)
#define AVBOX_PLAYERCTL_SEEK				(0x04)
#define AVBOX_PLAYERCTL_THREADEXIT			(0x05)
#define AVBOX_PLAYERCTL_STREAM_READY			(0x06)
#define AVBOX_PLAYERCTL_AUDIODEC_READY			(0x07)
#define AVBOX_PLAYERCTL_VIDEODEC_READY			(0x08)
#define AVBOX_PLAYERCTL_AUDIOOUT_READY			(0x09)
#define AVBOX_PLAYERCTL_VIDEOOUT_READY 			(0x0A)
#define AVBOX_PLAYERCTL_STREAM_EXIT			(0x0B)
#define AVBOX_PLAYERCTL_BUFFER_UNDERRUN			(0x0C)
#define AVBOX_PLAYERCTL_AUDIO_STREAM_UNDERRUN		(0x0D)
#define AVBOX_PLAYERCTL_ASPECT_RATIO_CHANGE		(0x0E)
#define AVBOX_PLAYERCTL_CHANGE_AUDIO_TRACK		(0x0F)
#define AVBOX_PLAYERCTL_FLUSH				(0x10)
#define AVBOX_PLAYERCTL_STILL_FRAME			(0x11)
#define AVBOX_PLAYERCTL_RESET_CLOCK			(0x12)
#define AVBOX_PLAYERCTL_SET_TITLE			(0x13)
#define AVBOX_PLAYERCTL_SET_DURATION			(0x14)
#define AVBOX_PLAYERCTL_SET_POSITION			(0x15)
#define AVBOX_PLAYERCTL_UPDATE				(0x16)


struct avbox_player;

enum avbox_aspect_ratio
{
	AVBOX_ASPECT_16_9 = 0,
	AVBOX_ASPECT_4_3 = 1
};


/* TODO: This belongs somewhere else */
struct avbox_size
{
	int w;
	int h;
};


LISTABLE_STRUCT(avbox_playlist_item,
	const char *filepath;
);


/**
 * Media player status enum.
 */
enum avbox_player_status
{
	MB_PLAYER_STATUS_READY,
	MB_PLAYER_STATUS_BUFFERING,
	MB_PLAYER_STATUS_PLAYING,
	MB_PLAYER_STATUS_PAUSED
};


/**
 * Status notification structure.
 */
struct avbox_player_status_data
{
	struct avbox_player *sender;
	enum avbox_player_status last_status;
	enum avbox_player_status status;
};


/**
 * Stores information about the current state of the
 * player.
 */
struct avbox_player_state_info
{
	int64_t pos;
	int64_t duration;
	char *title;
	struct avbox_size video_res;
	struct avbox_size scaled_res;	/* this is the scaled video size */
	enum avbox_aspect_ratio aspect_ratio;
};


/* status changed callback function */
typedef void (*avbox_player_status_callback)(struct avbox_player *inst,
	enum avbox_player_status status, enum avbox_player_status last_status);

/**
 * Subscribe to receive player notifications.
 */
int
avbox_player_subscribe(struct avbox_player * const inst,
	struct avbox_object * const object);


/**
 * Unsubscribe from player events.
 */
int
avbox_player_unsubscribe(struct avbox_player * const inst,
	struct avbox_object * const object);


/**
 * Plays a list of items
 */
int
avbox_player_playlist(struct avbox_player* inst, LIST *playlist, struct avbox_playlist_item* item);


/**
 * Get the current status of a media player instance.
 */
enum avbox_player_status
avbox_player_getstatus(struct avbox_player* inst);


/**
 * Get the last played file
 */
char *
avbox_player_getmediafile(struct avbox_player *inst);


/**
 * Gets the title of the currently playing
 * media file or NULL if nothing is playing. The result needs to be
 * freed with free().
 */
char *
avbox_player_gettitle(struct avbox_player *inst);


/**
 * Get the media duration.
 */
void
avbox_player_getduration(struct avbox_player * const inst, int64_t *duration);


/**
 * Get the current player window.
 */
struct avbox_window *
avbox_player_window(struct avbox_player * const inst);

/**
 * Get the media position in microseconds.
 */
void
avbox_player_gettime(struct avbox_player * const inst, int64_t *time);


/**
 * Get the state of the stream buffer
 */
unsigned int
avbox_player_bufferstate(struct avbox_player *inst);


/**
 * Seek to a chapter.
 */
void
avbox_player_seek(struct avbox_player *inst, int flags, int64_t pos);


/**
 * Tell the player to switch audio stream.
 */
void
avbox_player_changeaudiotrack(struct avbox_player * const inst, int track_id);


/**
 * Play a file.
 */
void
avbox_player_play(struct avbox_player* inst, const char * const path);


void
avbox_player_pause(struct avbox_player* inst);


void
avbox_player_stop(struct avbox_player* inst);


/**
 * Gets the underlying dispatch object.
 */
struct avbox_object *
avbox_player_object(struct avbox_player * const inst);


/**
 * Create a new media player instance.
 */
struct avbox_player*
avbox_player_new(struct avbox_window *window);


/**
 * Sends a control message to the player.
 */
int
avbox_player_sendctl(struct avbox_player * const inst,
	const int ctl, void * const data);


#endif
