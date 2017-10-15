/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
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
