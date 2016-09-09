#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/rfcomm.h>

#include <glib.h>
#include <gio/gio.h>

#include "input.h"
#include "input-socket.h"
#include "linkedlist.h"
#include "debug.h"


#define BLUEZ_BUS_NAME "org.bluez"
#define BLUEZ_INTF_ADAPTER "org.bluez.Adapter"


static int sockfd = -1;
static int newsockfd = -1;
static int server_quit = 0;
static pthread_t thread;
static GMainLoop *main_loop = NULL;
static GDBusConnection *dbus_conn = NULL;


LIST_DECLARE_STATIC(sockets);


static void
mbi_bluetooth_socket_closed(struct conn_state *state)
{
	DEBUG_VPRINT("input-bluetooth", "Connection closed (fd=%i)", state->fd);
	LIST_REMOVE(state);
	free(state);
}


static sdp_session_t *
mbi_bluetooth_register_service(uint8_t rfcomm_channel)
{
	uint32_t service_uuid_int[] = { 0, 0, 0, 0xABCD };
	const char *service_name = PACKAGE_NAME " Input Service";
	const char *service_dsc = PACKAGE_NAME " Remote Control Interface";
	const char *service_prov = PACKAGE_NAME;;
	int err = 0;
	sdp_session_t *session = 0;

	uuid_t root_uuid, l2cap_uuid, rfcomm_uuid, svc_uuid;
	sdp_list_t *l2cap_list = NULL, *rfcomm_list = NULL, *root_list = NULL;
	sdp_list_t *proto_list = NULL, *access_proto_list = NULL;
	sdp_data_t *channel = 0;
	sdp_record_t *record = sdp_record_alloc();

	/* set the general service ID */
	sdp_uuid128_create(&svc_uuid, &service_uuid_int);
	sdp_set_service_id(record, svc_uuid);

	/* make the service record publicly browsable */
	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root_list = sdp_list_append(0, &root_uuid);
	sdp_set_browse_groups(record, root_list);

	/* set l2cap information */
	sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
	l2cap_list = sdp_list_append(0, &l2cap_uuid);
	proto_list = sdp_list_append(0, l2cap_list);

	/* set rfcomm information */
	sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
	channel = sdp_data_alloc(SDP_UINT8, &rfcomm_channel);
	rfcomm_list = sdp_list_append(0, &rfcomm_uuid);
	sdp_list_append(rfcomm_list, channel);
	sdp_list_append(proto_list, rfcomm_list);

	/* attach protocol information to service record */
	access_proto_list = sdp_list_append(0, proto_list);
	sdp_set_access_protos(record, access_proto_list);

	/* set the name, provider, and description */
	sdp_set_info_attr(record, service_name, service_prov, service_dsc);

	/* connect to the local SDP server, register the service record, and
	 * disconnect */
	session = sdp_connect(BDADDR_ANY, BDADDR_LOCAL, SDP_RETRY_IF_BUSY);
	if (session == NULL) {
		DEBUG_VPRINT("input-bluetooth", "sdp_connect() returned NULL (errno=%i)\n",
			errno);
		return NULL;
	}
	if ((err = sdp_record_register(session, record, 0)) != 0) {
		DEBUG_VPRINT("input-bluetooth", "sdp_record_register() returned %i", err);
		return NULL;
	}

	/* cleanup */
	sdp_data_free(channel);
	sdp_list_free(l2cap_list, 0);
	sdp_list_free(rfcomm_list, 0);
	sdp_list_free(root_list, 0);
	sdp_list_free(access_proto_list, 0);

	return session;
}


static void
mbi_bluetooth_poweron()
{
	return;
}


static void *
mbi_bluetooth_server(void *arg)
{
	int channelno;
	unsigned int clilen;
	struct sockaddr_rc serv_addr, cli_addr;
	struct timeval tv;
	struct conn_state *state = NULL;
	fd_set fds;
	int n;

	MB_DEBUG_SET_THREAD_NAME("input-bluetooth");
	DEBUG_PRINT("input-bluetooth", "Bluetooth input server starting");

	while (!server_quit) {
		channelno = 1;
		sockfd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
		if (sockfd < 0) {
			fprintf(stderr, "mbi_bluetooth: Could not open socket\n");
			sleep(1);
			continue;
		}
rebind:
		bzero((char *) &serv_addr, sizeof(serv_addr));
		serv_addr.rc_family = AF_BLUETOOTH;
		serv_addr.rc_channel = channelno;
		serv_addr.rc_bdaddr = *BDADDR_ANY;

		if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
			fprintf(stderr, "mbi_bluetooth: Could not bind to socket (channel=%i)\n",
				channelno);
			if (channelno < 30) {
				channelno++;
				goto rebind;
			}
			close(sockfd);
			sleep(5);
			continue;
		}

		if (listen(sockfd, 1) == -1) {
			fprintf(stderr, "input-bluetooth: listen() failed\n");
			close(sockfd);
			sleep(5);
			continue;
		}
		clilen = sizeof(cli_addr);

		/* register the bluetooth service */
		mbi_bluetooth_register_service(channelno);

		DEBUG_VPRINT("input-bluetooth", "Listening for connections on RFCOMM channel %i",
			channelno);

		while(!server_quit) {

			FD_ZERO(&fds);
			FD_SET(sockfd, &fds);

			/* fprintf(stderr, "input-bluetooth: Waiting for connection\n"); */

			tv.tv_sec = 1;
			tv.tv_usec = 0;
			if ((n = select(sockfd + 1, &fds, NULL, NULL, &tv)) == 0) {
				continue;
			} else if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				fprintf(stderr, "input-bluetooth: select() returned %i\n", n);
				break;
			}

			if (!FD_ISSET(sockfd, &fds)) {
				continue;
			}

			if ((newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen)) <= 0) {
				fprintf(stderr, "input-bluetooth: Could not accept socket. ret=%i\n",
					newsockfd);
				continue;
			}

			DEBUG_VPRINT("input-bluetooth", "Incoming connection accepted (fd=%i)", newsockfd);

			if ((state = malloc(sizeof(struct conn_state))) == NULL) {
				fprintf(stderr, "input-bluetooth: Could not allocate connection state: Out of memory\n");
				close(newsockfd);
				continue;
			}

			state->fd = newsockfd;
			state->quit = 0;
			state->closed_callback = mbi_bluetooth_socket_closed;

			LIST_ADD(&sockets, state);

			if (pthread_create(&state->thread, NULL, &mbi_socket_connection, state) != 0) {
				fprintf(stderr, "input-bluetooth: Could not launch connection thread\n");
				close(newsockfd);
				free(state);
				continue;
			}
		}
		close(sockfd);
		sockfd = -1;
	}

	DEBUG_PRINT("input-bluetooth", "TCP input server exiting");

	return NULL;
}


/**
 * mbi_bluetooth_init() -- Initialize the tcp input server
 */
int
mbi_bluetooth_init(void)
{
	GError *error = NULL;

	LIST_INIT(&sockets);

	main_loop = g_main_loop_new(NULL, FALSE);
	if (main_loop == NULL) {
		fprintf(stderr, "input-bluetooth: Could not get new main loop\n");
		return -1;
	}

	/* connect to dbus system instance */
	dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (error) {
		fprintf(stderr, "input-bluetooth: Unable to get dbus connection\n");
		return -1;
	}

	/* power on bluetooth device */
	mbi_bluetooth_poweron();

	if (pthread_create(&thread, NULL, mbi_bluetooth_server, NULL) != 0) {
		fprintf(stderr, "input-bluetooth: Could not create bluetooth server thread\n");
		return -1;
	}
	return 0;
}


void
mbi_bluetooth_destroy(void)
{
	struct conn_state *socket;

	DEBUG_PRINT("input-bluetooth", "Exiting (give me 2 secs)");

	if (main_loop != NULL) {
		DEBUG_PRINT("input-bluetooth:", "TODO: Destroy main loop");
	}

	/* close the dbus connection */
	if (dbus_conn != NULL) {
		g_dbus_connection_close(dbus_conn, NULL, NULL, NULL);
		dbus_conn = NULL;
	}

	/* Close all connections */
	DEBUG_PRINT("input-bluetooth", "Closing all open sockets");
	LIST_FOREACH(struct conn_state*, socket, &sockets) {
		socket->quit = 1;
		pthread_join(socket->thread, NULL);
	}

	server_quit = 1;
	if (newsockfd != -1) {
		close(newsockfd);
	}
	close(sockfd);
	pthread_join(thread, NULL);
}

