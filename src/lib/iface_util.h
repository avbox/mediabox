/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __IFACE_UTIL_H__
#define __IFACE_UTIL_H__

/**
 * Callback for enumerating network interfaces
 */
typedef int (*ifaceutil_enum_callback)(const char * const iface_name, void *data);


/*
 * ifaceutil_getip() -- Gets the IP address of a network
 * interface
 */
char*
ifaceutil_getip(const char * const iface_name);


/**
 * ifaceutil_enumifaces() -- Enumerate network interfaces
 */
int
ifaceutil_enumifaces(ifaceutil_enum_callback callback, void *data);

#endif
