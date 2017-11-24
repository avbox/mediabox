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


#ifndef __AVBOX_H__
#define __AVBOX_H__

#ifdef HAVE_CONFIG_H
#	include "../config.h"
#endif


#include "log.h"
#include "debug.h"
#include "queue.h"
#include "timers.h"
#include "dispatch.h"
#include "ui/video.h"
#include "ui/input.h"
#include "ui/player.h"

#include "su.h"
#include "time_util.h"
#include "timers.h"
#include "linkedlist.h"
#include "audio.h"
#include "compiler.h"
#include "queue.h"
#include "dispatch.h"
#include "application.h"
#include "math_util.h"
#include "ffmpeg_util.h"
#include "checkpoint.h"
#include "thread.h"
#include "stopwatch.h"
#include "syncarg.h"

#ifdef ENABLE_DVD
#	include "dvdio.h"
#endif


#endif
