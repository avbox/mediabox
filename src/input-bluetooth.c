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

#include "input.h"
#include "input-socket.h"
#include "linkedlist.h"
#include "debug.h"


static int sockfd = -1;
static int newsockfd = -1;
static int server_quit = 0;
static pthread_t thread;


LIST_DECLARE_STATIC(sockets);


static void
mbi_bluetooth_socket_closed(struct conn_state *state)
{
	DEBUG_VPRINT("input-bluetooth", "Connection closed (fd=%i)", state->fd);
	LIST_REMOVE(state);
	free(state);
}


static sdp_session_t *
mbi_bluetooth_register_service()
{
	uint32_t service_uuid_int[] = { 0, 0, 0, 0xABCD };
	uint8_t rfcomm_channel = 11;
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


static void *
mbi_bluetooth_server(void *arg)
{
	int portno;
	unsigned int clilen;
	struct sockaddr_in serv_addr, cli_addr;
	struct timeval tv;
	struct conn_state *state = NULL;
	fd_set fds;
	int n;

	MB_DEBUG_SET_THREAD_NAME("input-bluetooth");
	DEBUG_PRINT("input-bluetooth", "Bluetooth input server starting");

	mbi_bluetooth_register_service();

	while (!server_quit) {

		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0) {
			fprintf(stderr, "mbi_bluetooth: Could not open socket\n");
			sleep(1);
			continue;
		}

		bzero((char *) &serv_addr, sizeof(serv_addr));
		portno = 2048;
		serv_addr.sin_family = AF_INET;
		serv_addr.sin_addr.s_addr = INADDR_ANY;
		serv_addr.sin_port = htons(portno);
		if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
			fprintf(stderr, "mbi_bluetooth: Could not bind to socket\n");
			close(sockfd);
			sockfd = -1;
			sleep(5);
			continue;
		}

		listen(sockfd, 1);
		clilen = sizeof(cli_addr);

		DEBUG_VPRINT("input-bluetooth", "Listening for connections on RFCOMM channel %i", portno);

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
	LIST_INIT(&sockets);

	if (pthread_create(&thread, NULL, mbi_bluetooth_server, NULL) != 0) {
		fprintf(stderr, "Could not create TCP server thread\n");
		return -1;
	}
	return 0;
}


void
mbi_bluetooth_destroy(void)
{
	struct conn_state *socket;

	DEBUG_PRINT("input-bluetooth", "Exiting (give me 2 secs)");

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

