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


#ifdef HAVE_CONFIG_H
#	include "../config.h"
#endif
#ifdef ENABLE_BLUETOOTH


/**
 * Represents a Bluetooth device
 */
struct avbox_btdev {
	char *name;
	char *address;
	int connected;
	int paired;
};



int
avbox_bluetooth_register_service(int rfcomm_channel);


/**
 * Free avbox_btdev structure.
 */
void
avbox_bluetooth_freedev(struct avbox_btdev *dev);


/**
 * Gets a list of devices.
 */
struct avbox_btdev **
avbox_bluetooth_getdevices(const char * uuid);


/**
 * Sets the power state of the adapter.
 */
int
avbox_bluetooth_setpower(int state);


/**
 * Initialize bluetooth subsystem.
 */
int
avbox_bluetooth_init(void);

void
avbox_bluetooth_shutdown(void);

#endif
