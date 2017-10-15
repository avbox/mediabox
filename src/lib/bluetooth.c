/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef ENABLE_BLUETOOTH
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/rfcomm.h>

#include <glib.h>
#include <gio/gio.h>

#define LOG_MODULE "bluetooth"

#include "linkedlist.h"
#include "queue.h"
#include "debug.h"
#include "log.h"
#include "process.h"
#include "bluetooth.h"


#define BLUETOOTHD_BIN "/usr/libexec/bluetooth/bluetoothd"
#define BLUEALSA_BIN "/usr/bin/bluealsa"
#define BLUEZ_BUS_NAME "org.bluez"
#define BLUEZ_INTF_ADAPTER "org.bluez.Adapter"
#define BLUEZ_INTF_AGENT "org.bluez.Agent1"
#define BLUEZ_AGENT_PATH "/org/mediabox"


static int bluetooth_daemon_id = -1;
static int bluealsa_daemon_id = -1;
static int btok = 0;
static guint agent_id = 0;
static GDBusConnection *dbus_conn = NULL;
static GDBusProxy *adapter_properties = NULL;
static GDBusProxy *agent_manager = NULL;
static GMainLoop *mainloop = NULL;
static pthread_t thread;
static pthread_mutex_t sync_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_condition = PTHREAD_COND_INITIALIZER;


/**
 * Handles bluetooth pairing requests.
 */
static void
avbox_bluetooth_agent(
	GDBusConnection *conn,
	const gchar *sender,
	const gchar *object_path,
	const gchar *interface_name,
	const gchar *method_name,
	GVariant *params,
	GDBusMethodInvocation *invocation,
	gpointer data)
{
	if (!strcmp(method_name, "AuthorizeService")) {

		DEBUG_PRINT("bluetooth", "Agent::AuthorizeService() called");
		g_dbus_method_invocation_return_value(invocation, NULL);

	} else if (!strcmp(method_name, "Cancel")) {

		DEBUG_PRINT("bluetooth", "Pairing request cancelled");
		g_dbus_method_invocation_return_value(invocation, NULL);

	} else if (!strcmp(method_name, "DisplayPasskey") ||
		!strcmp(method_name, "DisplayPinCode") ||
		!strcmp(method_name, "RequestAuthorization") ||
		!strcmp(method_name, "RequestConfirmation")) {

		DEBUG_VPRINT("bluetooth", "Agent::%s() called",
			method_name);

		/* just accept the passkey */
		g_dbus_method_invocation_return_value(invocation, NULL);

	} else if (!strcmp(method_name, "Release")) {

		DEBUG_PRINT("bluetooth", "Agent released");
		g_dbus_method_invocation_return_value(invocation, NULL);

	} else if (!strcmp(method_name, "RequestPassKey")) {

		DEBUG_PRINT("bluetooth", "Agent::RequestPassKey() called");
		g_dbus_method_invocation_return_dbus_error(invocation,
			"org.bluez.Error.Rejected", "No passkey entered");

	} else if (!strcmp(method_name, "RequestPinCode")) {

		DEBUG_PRINT("bluetooth", "Agent::RequestPinCode() called");
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(s)", "000000"));

	} else {

		DEBUG_VPRINT("bluetooth", "Unimplemented method Agent::%s() called!",
			method_name);

		g_dbus_method_invocation_return_dbus_error(invocation,
			"org.bluez.Error.Rejected", "Method not implemented");
	}
}


static void
avbox_bluetooth_destroyagent(gpointer data)
{
	g_free(data);
}


/**
 * Register the agent.
 */
static int
avbox_bluetooth_registeragent()
{
	static const gchar *agent_xml =
		"<node name=\"/org/mediabox\"> \
			<interface name=\"org.bluez.Agent1\">\
				<method name=\"Release\"></method>\
				<method name=\"RequestPinCode\">\
					<arg name=\"device\" direction=\"in\" type=\"o\"/>\
					<arg name=\"pincode\" direction=\"out\" type=\"s\"/>\
				</method>\
				<method name=\"DisplayPinCode\">\
					<arg name=\"device\" direction=\"in\" type=\"o\"/>\
					<arg name=\"pincode\" direction=\"in\" type=\"s\"/>\
				</method>\
				<method name=\"RequestPasskey\">\
					<arg name=\"device\" direction=\"in\" type=\"o\"/>\
					<arg name=\"passkey\" direction=\"out\" type=\"u\"/>\
				</method>\
				<method name=\"DisplayPasskey\">\
					<arg name=\"device\" direction=\"in\" type=\"o\"/>\
					<arg name=\"passkey\" direction=\"in\" type=\"u\"/>\
					<arg name=\"entered\" direction=\"in\" type=\"q\"/>\
				</method>\
				<method name=\"RequestConfirmation\">\
					<arg name=\"device\" direction=\"in\" type=\"o\"/>\
					<arg name=\"passkey\" direction=\"in\" type=\"u\"/>\
				</method>\
				<method name=\"RequestAuthorization\">\
					<arg name=\"device\" direction=\"in\" type=\"o\"/>\
				</method>\
				<method name=\"AuthorizeService\">\
					<arg name=\"device\" direction=\"in\" type=\"o\"/>\
					<arg name=\"uuid\" direction=\"in\" type=\"s\"/>\
				</method>\
				<method name=\"Cancel\"></method>\
			</interface>\
		</node>";
	int ret = 0;
	GError *error = NULL;
	GDBusInterfaceVTable agent_vtable;
	GDBusNodeInfo *node;
	GDBusInterfaceInfo *interface;

	memset(&agent_vtable, 0, sizeof(GDBusInterfaceVTable));
	node = g_dbus_node_info_new_for_xml(agent_xml, &error);
	if (node == NULL) {
		ASSERT(error != NULL);
		LOG_VPRINT_ERROR("Could not parse node info: %s",
			error->message);
		g_clear_error(&error);
		return -1;
	}

	interface = g_dbus_node_info_lookup_interface(node, BLUEZ_INTF_AGENT);
	agent_vtable.method_call = avbox_bluetooth_agent;
	agent_id = g_dbus_connection_register_object(dbus_conn, BLUEZ_AGENT_PATH,
		interface, &agent_vtable, NULL, avbox_bluetooth_destroyagent,
		&error);
	if (error != NULL) {
		LOG_VPRINT_ERROR("Could not register bluetooth agent service: %s",
			error->message);
		g_clear_error(&error);
		ret = -1;
		/* fall through */
	}

	g_dbus_node_info_unref(node);

	/* register the agent with bluez */
	g_dbus_proxy_call_sync(agent_manager, "RegisterAgent",
		g_variant_new("(os)", BLUEZ_AGENT_PATH, "DisplayOnly"),
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (error != NULL) {
		LOG_VPRINT_ERROR("Could not register agent with bluez: %s",
			error->message);
		g_clear_error(&error);
		goto end;
	}

	/* request default agent */
	g_dbus_proxy_call_sync(agent_manager, "RequestDefaultAgent",
		g_variant_new("(o)", BLUEZ_AGENT_PATH),
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (error != NULL) {
		LOG_VPRINT_ERROR("Could not request default agent: %s",
			error->message);
		g_clear_error(&error);
	}

end:
	return ret;
}


/**
 * Unregisters the bluetooth agent.
 */
static void
avbox_bluetooth_unregisteragent()
{
	if (agent_id != 0) {
		g_dbus_connection_unregister_object(dbus_conn, agent_id);
		agent_id = 0;
	}
}


int
avbox_bluetooth_register_service(int rfcomm_channel)
{
	return 0;
}


/**
 * Free an avbox_btdev structure.
 */
void
avbox_bluetooth_freedev(struct avbox_btdev *dev)
{
	if (dev->name != NULL) {
		free(dev->name);
	}
	if (dev->address != NULL) {
		free(dev->address);
	}
	free(dev);
}


/**
 * Gets a list of devices.
 */
struct avbox_btdev **
avbox_bluetooth_getdevices(const char * uuid)
{
	GError *error = NULL;
	GDBusProxy *proxy;
	GVariant *retval, *objs;
	GVariantIter iter1;
	const gchar *object_path;
	GVariant *ifaces_n_props;
	struct avbox_queue *queue;
	struct avbox_btdev **results = NULL;

	DEBUG_VPRINT("bluetooth", "Querying devices with UUID: %s",
		uuid);

	/* create a proxy for the object manager */
	proxy = g_dbus_proxy_new_sync(dbus_conn,
		G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez", "/",
		"org.freedesktop.DBus.ObjectManager", NULL, &error);
	if (proxy == NULL) {
		ASSERT(error != NULL);
		LOG_PRINT_ERROR("Could not create object manager proxy");
		return NULL;
	}

	/* create a queue to temporarily store the devices */
	if ((queue = avbox_queue_new(0)) == NULL) {
		LOG_VPRINT_ERROR("Could not create queue: %s",
			strerror(errno));
		return NULL;
	}

	/* query all the objects */
	retval = g_dbus_proxy_call_sync(proxy, "GetManagedObjects", NULL,
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (retval == NULL) {
		ASSERT(error != NULL);
		LOG_VPRINT_ERROR("Could not get managed objects: %s",
			error->message);
		g_clear_error(&error);
		goto end;
	}

	objs = g_variant_get_child_value(retval, 0);

	g_variant_iter_init(&iter1, objs);
	while (g_variant_iter_next(&iter1, "{&o@a{sa{sv}}}", &object_path, &ifaces_n_props)) {
		const gchar *iface_name;
		GVariant *props;
		GVariantIter iter2;

		g_variant_iter_init(&iter2, ifaces_n_props);

		while (g_variant_iter_next(&iter2, "{&s@a{sv}}", &iface_name, &props)) {
			if (!strcmp(iface_name, "org.bluez.Device1")) {
				GDBusProxy *dev;
				GVariant *name = NULL, *address = NULL,
					*connected = NULL, *uuids = NULL;
				struct avbox_btdev *btdev;

				dev = g_dbus_proxy_new_sync(dbus_conn,
					G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez", object_path,
					"org.freedesktop.DBus.Properties", NULL, &error);
				if (dev == NULL) {
					ASSERT(error != NULL);
					LOG_VPRINT_ERROR("Could not create proxy for %s: %s",
						object_path, error->message);
					g_clear_error(&error);
					continue;
				}

				name = g_dbus_proxy_call_sync(dev, "Get",
					g_variant_new("(ss)", iface_name, "Name"),
					G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
				if (name == NULL) {
					LOG_VPRINT_ERROR("Could not get Name property: %s",
						error->message);
					g_clear_error(&error);
					goto enddev;
				}

				address = g_dbus_proxy_call_sync(dev, "Get",
					g_variant_new("(ss)", iface_name, "Address"),
					G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
				if (address == NULL) {
					LOG_VPRINT_ERROR("Could not get Address property: %s",
						error->message);
					g_clear_error(&error);
					goto enddev;
				}

				connected = g_dbus_proxy_call_sync(dev, "Get",
					g_variant_new("(ss)", iface_name, "Connected"),
					G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
				if (connected == NULL) {
					LOG_VPRINT_ERROR("Could not get Connected property: %s",
						error->message);
					g_clear_error(&error);
					goto enddev;
				}

				uuids = g_dbus_proxy_call_sync(dev, "Get",
					g_variant_new("(ss)", iface_name, "UUIDs"),
					G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
				if (uuids == NULL) {
					LOG_VPRINT_ERROR("Could not get UUIDS list: %s",
						error->message);
					g_clear_error(&error);
					goto enddev;
				}

				/* unpack all property values */
				name = g_variant_get_child_value(name, 0);
				name = g_variant_get_variant(name);
				address = g_variant_get_child_value(address, 0);
				address = g_variant_get_variant(address);
				connected = g_variant_get_child_value(connected, 0);
				connected = g_variant_get_variant(connected);
				uuids = g_variant_get_child_value(uuids, 0);
				uuids = g_variant_get_variant(uuids);

				/* if a uuid argument was given then we only
				 * want the devices that have that uuid */
				if (uuid != NULL && uuids != NULL) {
					GVariant *uuid_val;
					GVariantIter uuids_iter;
					int have_uuid = 0;
					g_variant_iter_init(&uuids_iter, uuids);
					while ((uuid_val = g_variant_iter_next_value(&uuids_iter))) {
						if (!strcmp(uuid, g_variant_get_string(uuid_val, NULL))) {
							have_uuid = 1;
						}
						g_variant_unref(uuid_val);
					}
					if (!have_uuid) {
						goto enddev;
					}
				}

				/* allocate a avbox_btdev struct, populate it
				 * and added to the queue */
				if ((btdev = malloc(sizeof(struct avbox_btdev))) == NULL) {
					LOG_VPRINT_ERROR("Could not allocate memory for btdev: %s",
						strerror(errno));
					goto enddev;
				}
				btdev->name = strdup(g_variant_get_string(name, NULL));
				btdev->address = strdup(g_variant_get_string(address, NULL));
				btdev->connected = g_variant_get_boolean(connected) ? 1 : 0;
				btdev->paired = 1;
				avbox_queue_put(queue, btdev);

enddev:
				if (uuids != NULL) {
					g_variant_unref(uuids);
				}
				if (address != NULL) {
					g_variant_unref(address);
				}
				if (name != NULL) {
					g_variant_unref(name);
				}
				g_object_unref(dev);
			}
		}
		g_variant_unref(props);
	}
	g_variant_unref(ifaces_n_props);
	g_variant_unref(objs);
end:
	g_object_unref(proxy);

	/* copy results from queue to an array. If we fail
	 * to allocate the array then free the results instead */
	if (avbox_queue_count(queue) > 0) {
		int c, cnt;
		if ((results = malloc(sizeof(struct avbox_btdev*) * (avbox_queue_count(queue) + 1))) == NULL) {
			LOG_PRINT_ERROR("Could not allocate device table!");
		}
		cnt = avbox_queue_count(queue);
		for (c = 0; c < cnt; c++) {
			struct avbox_btdev *btdev = avbox_queue_get(queue);
			if (results != NULL) {
				results[c] = btdev;
			} else {
				avbox_bluetooth_freedev(btdev);
			}
		}
		results[c] = NULL;
	}

	avbox_queue_destroy(queue);

	return results;
}


/**
 * Checks if the Bluetooth adapter is ready.
 */
int
avbox_bluetooth_ready()
{
	return btok;
}


/**
 * Sets the power state of the adapter.
 */
int
avbox_bluetooth_setpower(int state)
{
	GError *error = NULL;

	DEBUG_VPRINT("bluetooth", "Setting adapter power to %s",
		state ? "on" : "off");

	/* power on the adapter */
	g_dbus_proxy_call_sync(adapter_properties,
		"Set", g_variant_new("(ssv)", "org.bluez.Adapter1",
		"Powered", g_variant_new_boolean(TRUE)),
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (error != NULL) {
		LOG_VPRINT_ERROR("Could not power bluetooth adapter: %s",
			error->message);
		g_clear_error(&error);
		return -1;
	}
	return 0;
}


/**
 * Sets the adapter's discoverable state.
 */
int
avbox_bluetooth_setdiscoverable(int state)
{
	GError *error = NULL;

	/* make it discoverable */
	g_dbus_proxy_call_sync(adapter_properties,
		"Set", g_variant_new("(ssv)", "org.bluez.Adapter1",
		"Discoverable", g_variant_new_boolean(TRUE)),
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (error != NULL) {
		LOG_VPRINT_ERROR("Could not make device discoverable: %s",
			error->message);
		g_clear_error(&error);
		return -1;
	}
	return 0;
}


static void *
avbox_bluetooth_mainloop(void * arg)
{
	DEBUG_SET_THREAD_NAME("bluetooth");
	DEBUG_PRINT("bluetooth", "Initializing bluetooth subsystem");
	ASSERT(btok == 0);

	GError *error = NULL;
	GDBusProxy *proxy;
	GVariant *introspection_gvar;
	gchar *introspection_xml;
	const char * const bluetoothd_args[] =
	{
		BLUETOOTHD_BIN,
		NULL
	};
	const char * const bluealsa_args[] =
	{
		BLUEALSA_BIN,
		"--disable-hsp",
		"--disable-hfp",
		NULL
	};

	(void) arg;

	/* make sure the main thread is already waiting
	 * before starting. This way we don't need to lock
	 * the mutex before signaling a condition */
	pthread_mutex_lock(&sync_mutex);
	pthread_mutex_unlock(&sync_mutex);

	/* launch the bluetoothd process */
	if ((bluetooth_daemon_id = avbox_process_start(BLUETOOTHD_BIN, bluetoothd_args,
		AVBOX_PROCESS_AUTORESTART | AVBOX_PROCESS_NICE | AVBOX_PROCESS_IONICE_IDLE |
		AVBOX_PROCESS_SUPERUSER, "bluetoothd", NULL, NULL)) == -1) {
		LOG_PRINT_ERROR("Could not start bluetooth daemon");
		pthread_cond_signal(&sync_condition);
		return NULL;
	}

	/* sleep for one second for bluetoothd to start before
	 * we try to use it */
	sleep(1);

	mainloop = g_main_loop_new(NULL, FALSE);
	ASSERT(mainloop != NULL);

	/* connect to dbus system instance */
	dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (error) {
		LOG_PRINT_ERROR("Could not connect to system dbus!");
		pthread_cond_signal(&sync_condition);
		return NULL;
	}

	/* create a proxy for the object manager for org.bluez */
	proxy = g_dbus_proxy_new_sync(dbus_conn,
		G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez", "/",
		"org.freedesktop.DBus.Introspectable", NULL, &error);
	ASSERT(proxy != NULL);

	/* call org.freedesktop.DBus.Introspectable on dbus object manager */
	introspection_gvar = g_dbus_proxy_call_sync(proxy,
		"Introspect", NULL, G_DBUS_CALL_FLAGS_NONE, -1,
		NULL, &error);
	if (introspection_gvar == NULL) {
		LOG_VPRINT_ERROR("Bluetooth service not running! Call to Introspect() failed: %s",
			error->message);
		g_dbus_connection_close(dbus_conn, NULL, NULL, NULL);
		dbus_conn = NULL;
		pthread_cond_signal(&sync_condition);
		return NULL;
	}
	ASSERT(error == NULL);
	introspection_xml = (gchar*) g_variant_get_string(
		g_variant_get_child_value(introspection_gvar, 0), NULL);
	if (!strstr(introspection_xml,
		"<interface name=\"org.freedesktop.DBus.ObjectManager\">")) {
		LOG_PRINT_ERROR("Bluetooth service not running!");
		LOG_VPRINT_ERROR("Introspect() returned %s",
			introspection_xml);
		g_variant_unref(introspection_gvar);
		g_clear_error(&error);
		pthread_cond_signal(&sync_condition);
		return NULL;
	}

	g_variant_unref(introspection_gvar);
	g_object_unref(proxy);

	/* create a proxy for the adapter hci0 properties interface */
	adapter_properties = g_dbus_proxy_new_sync(dbus_conn,
		G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez", "/org/bluez/hci0",
		"org.freedesktop.DBus.Properties", NULL, &error);
	if (adapter_properties == NULL) {
		ASSERT(error != NULL);
		LOG_VPRINT_ERROR("Could not create dbus proxy for adapter object: %s",
			error->message);
		LOG_PRINT_ERROR("Could not poweron bluetooth adapter hci0");
		g_clear_error(&error);
		pthread_cond_signal(&sync_condition);
		return NULL;
	}

	ASSERT(error == NULL);

	/* create a proxy for the agent manager */
	agent_manager = g_dbus_proxy_new_sync(dbus_conn,
		G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez", "/org/bluez",
		"org.bluez.AgentManager1", NULL, &error);
	if (agent_manager == NULL) {
		ASSERT(error != NULL);
		LOG_VPRINT_ERROR("Could not create dbus proxy for agent manager: %s",
			error->message);
		g_clear_error(&error);
		pthread_cond_signal(&sync_condition);
		return NULL;
	}

	/* bluetooth has been initialized. signal
	 * the main thread to continue */
	DEBUG_PRINT("bluetooth", "Bluetooth subsystem initialized");
	btok = 1;
	pthread_cond_signal(&sync_condition);

	/* power on bluetooth device and make
	 * it discoverable */
	avbox_bluetooth_setpower(1);
	avbox_bluetooth_setdiscoverable(1);
	avbox_bluetooth_registeragent();

	/* launch the bluealsa process */
	if ((bluealsa_daemon_id = avbox_process_start(BLUEALSA_BIN, bluealsa_args,
		AVBOX_PROCESS_AUTORESTART | AVBOX_PROCESS_NICE | AVBOX_PROCESS_IONICE_IDLE |
		AVBOX_PROCESS_SUPERUSER, "bluealsa", NULL, NULL)) == -1) {
		LOG_PRINT_ERROR("WARNING!!: Could not start bluealsa daemon");
	}

	g_main_loop_run(mainloop);

	DEBUG_PRINT("bluetooth", "Bluetooth thread exiting");

	return NULL;
}


/**
 * Initialize bluetooth subsystem.
 */
int
avbox_bluetooth_init(void)
{
	DEBUG_PRINT("bluetooth", "Starting bluetooth thread");

	if (pthread_mutex_init(&sync_mutex, NULL) != 0 ||
		pthread_cond_init(&sync_condition, NULL) != 0) {
		LOG_PRINT_ERROR("Could not initialize pthread primitives");
		return -1;
	}

	/* Launch the bluetooth thread and wait for it
	 * to initialize the bluetooth subsystem */
	pthread_mutex_lock(&sync_mutex);
	if (pthread_create(&thread, NULL, avbox_bluetooth_mainloop, NULL) != 0) {
		LOG_PRINT_ERROR("Could not start bluetooth thread");
		return -1;
	}
	pthread_cond_wait(&sync_condition, &sync_mutex);
	pthread_mutex_unlock(&sync_mutex);
	if (!btok) {
		LOG_PRINT_ERROR("Bluetooth thread exitted abnormally");
		return -1;
	}

	return 0;
}


/**
 * Shutdown the bluetooth subsystem.
 */
void
avbox_bluetooth_shutdown(void)
{
	DEBUG_PRINT("bluetooth", "Shutting down bluetooth subsystem");

	avbox_bluetooth_unregisteragent();

	if (mainloop != NULL) {
		if (g_main_loop_is_running(mainloop)) {
			g_main_loop_quit(mainloop);
		}
	}

	pthread_join(thread, NULL);

	if (agent_manager != NULL) {
		g_object_unref(agent_manager);
		agent_manager = NULL;
	}

	/* release adapter properties interface */
	if (adapter_properties != NULL) {
		g_object_unref(adapter_properties);
		adapter_properties = NULL;
	}

	/* close the dbus connection */
	if (dbus_conn != NULL) {
		g_dbus_connection_close(dbus_conn, NULL, NULL, NULL);
		dbus_conn = NULL;
	}

	/* kill daemons */
	if (bluealsa_daemon_id != -1) {
		avbox_process_stop(bluealsa_daemon_id);
		bluealsa_daemon_id = -1;
	}
	if (bluetooth_daemon_id != -1) {
		avbox_process_stop(bluetooth_daemon_id);
		bluetooth_daemon_id = -1;
	}
}

#endif
