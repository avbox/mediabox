/**
 * Copyright (c) 2016-2018 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __IFACE_UTIL_H__
#define __IFACE_UTIL_H__

/**
 * Callback for enumerating network interfaces
 */
typedef int (*avbox_ifaceutil_enum_callback)(const char * const iface_name, void *data);


/**
 * Gets the IP address of a network interface
 */
char*
avbox_ifaceutil_getip(const char*const iface_name);


/**
 * Enumerate network interfaces
 */
int
avbox_ifaceutil_enumifaces(avbox_ifaceutil_enum_callback callback, void *data);

#endif
