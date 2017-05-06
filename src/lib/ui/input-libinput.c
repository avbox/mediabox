#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <libinput.h>

#define LOG_MODULE "input-libinput"

#include "input.h"
#include "../debug.h"
#include "../log.h"


static int quit = 0;
static struct libinput *li = NULL;
static pthread_t event_loop_thread;

#define DIKS_ESCAPE 		(1)
#define DIKS_RETURN 		(28)
#define DIKS_SHIFT 		(42)
#define DIKS_CURSOR_UP 		(103)
#define DIKS_CURSOR_DOWN 	(108)
#define DIKS_CURSOR_LEFT 	(105)
#define DIKS_CURSOR_RIGHT 	(106)
#define DIKS_BACKSPACE 		(14)
#define DIKS_SPACE		(57) 
#define DIKS_LEFT_CTRL		(29)
#define DIKS_LEFT_ALT		(56)

#define DIKS_KEY_A		(30)
#define DIKS_KEY_B		(48)
#define DIKS_KEY_C		(46)
#define DIKS_KEY_D		(32)
#define DIKS_KEY_E		(18)
#define DIKS_KEY_F		(33)
#define DIKS_KEY_G		(34)
#define DIKS_KEY_H		(35)
#define DIKS_KEY_I		(23)
#define DIKS_KEY_J		(36)
#define DIKS_KEY_K		(37)
#define DIKS_KEY_L		(38)
#define DIKS_KEY_M		(50)
#define DIKS_KEY_N		(49)
#define DIKS_KEY_O		(24)
#define DIKS_KEY_P		(25)
#define DIKS_KEY_Q		(16)
#define DIKS_KEY_R		(19)
#define DIKS_KEY_S		(31)
#define DIKS_KEY_T		(20)
#define DIKS_KEY_U		(22)
#define DIKS_KEY_V		(47)
#define DIKS_KEY_W		(17)
#define DIKS_KEY_X		(45)
#define DIKS_KEY_Y		(21)
#define DIKS_KEY_Z		(44)

#define CASE_KEYBOARD(x) \
	case DIKS_KEY_ ## x: \
		avbox_input_sendevent(MBI_EVENT_KBD_ ## x); \
		break;


/**
 * Runs the libinput driver event loop
 */
static void*
mbi_libinput_event_loop(void *arg)
{
	struct libinput_event *ev;
	struct pollfd fds;

	assert(li != NULL);

	(void) arg;


	DEBUG_PRINT("input-libinput", "Running libinput event loop");
	MB_DEBUG_SET_THREAD_NAME("input-libinput");

	fds.fd = libinput_get_fd(li);
	fds.events = POLLIN;
	fds.revents = 0;

	do {
		libinput_dispatch(li);
		while ((ev = libinput_get_event(li)) != NULL) {
			enum libinput_event_type et = libinput_event_get_type(ev);
			switch (et) {
			case LIBINPUT_EVENT_KEYBOARD_KEY:
			{
				uint32_t keycode;
				struct libinput_event_keyboard *evkey;
				enum libinput_key_state keystate;
				evkey = libinput_event_get_keyboard_event(ev);
				keycode = libinput_event_keyboard_get_key(evkey);
				keystate = libinput_event_keyboard_get_key_state(evkey);

				/* only react when the key is pressed */
				if (keystate == LIBINPUT_KEY_STATE_RELEASED) {
					libinput_event_destroy(ev);
					continue;
				}

				switch (keycode) {
				case DIKS_ESCAPE:       avbox_input_sendevent(MBI_EVENT_BACK); break;
				case DIKS_RETURN:       avbox_input_sendevent(MBI_EVENT_ENTER); break;
				case DIKS_SHIFT:        avbox_input_sendevent(MBI_EVENT_MENU); break;
				case DIKS_CURSOR_UP:    avbox_input_sendevent(MBI_EVENT_ARROW_UP); break;
				case DIKS_CURSOR_DOWN:  avbox_input_sendevent(MBI_EVENT_ARROW_DOWN); break;
				case DIKS_CURSOR_LEFT:  avbox_input_sendevent(MBI_EVENT_ARROW_LEFT); break;
				case DIKS_CURSOR_RIGHT: avbox_input_sendevent(MBI_EVENT_ARROW_RIGHT); break;
				case DIKS_BACKSPACE:    avbox_input_sendevent(MBI_EVENT_CLEAR); break;
				case DIKS_SPACE:        avbox_input_sendevent(MBI_EVENT_KBD_SPACE); break;
				case DIKS_LEFT_CTRL:	avbox_input_sendevent(MBI_EVENT_VOLUME_UP); break;
				case DIKS_LEFT_ALT:	avbox_input_sendevent(MBI_EVENT_VOLUME_DOWN); break;

				CASE_KEYBOARD(A)
				CASE_KEYBOARD(B)
				CASE_KEYBOARD(C)
				CASE_KEYBOARD(D)
				CASE_KEYBOARD(E)
				CASE_KEYBOARD(F)
				CASE_KEYBOARD(G)
				CASE_KEYBOARD(H)
				CASE_KEYBOARD(I)
				CASE_KEYBOARD(J)
				CASE_KEYBOARD(K)
				CASE_KEYBOARD(L)
				CASE_KEYBOARD(M)
				CASE_KEYBOARD(N)
				CASE_KEYBOARD(O)
				CASE_KEYBOARD(P)
				CASE_KEYBOARD(Q)
				CASE_KEYBOARD(R)
				CASE_KEYBOARD(S)
				CASE_KEYBOARD(T)
				CASE_KEYBOARD(U)
				CASE_KEYBOARD(V)
				CASE_KEYBOARD(W)
				CASE_KEYBOARD(X)
				CASE_KEYBOARD(Y)
				CASE_KEYBOARD(Z)

				default:
					DEBUG_VPRINT("input-libinput", "Key %u pressed",
						keycode);
					break;
				}
				break;
			}
			default:
				DEBUG_VPRINT("input-libinput", "Unknown event type: %d",
					(unsigned int) et);
				break;
			}
			libinput_event_destroy(ev);
		}
	}
	while (poll(&fds, 1, 250) > -1 && !quit);

	DEBUG_PRINT("input-libinput", "Exiting libinput event loop");

	return NULL;
}


static int 
open_restricted(const char *path, int flags, void *user_data)
{
        int fd = open(path, flags);
        if (fd < 0) {
		LOG_VPRINT_ERROR("Failed to open %s (%s)",
			path, strerror(errno));
	}
        return fd < 0 ? -errno : fd; 
}


static void
close_restricted(int fd, void *user_data)
{
        close(fd);
}


static const struct libinput_interface interface = { 
        .open_restricted = open_restricted,
        .close_restricted = close_restricted,
};


/**
 * Initialize the libinput driver.
 */
int
mbi_libinput_init(void)
{
	DIR *devinput;
	struct dirent *ent;
	struct libinput_device *dev;
	char devpath[32];

	DEBUG_PRINT("input-libinput", "Initializing libinput driver");

	li = libinput_path_create_context(&interface, NULL);
	if (li == NULL) {
		LOG_PRINT_ERROR("Could not create libinput context!");
		return -1;
	}

	if ((devinput = opendir("/dev/input")) == NULL) {
		LOG_PRINT_ERROR("Could not open directory /dev/input!");
		return -1;
	}

	while ((ent = readdir(devinput)) != NULL) {
		if (ent->d_name[0] == '.') {
			continue;
		}
		if (strlen(ent->d_name) > 8) {
			LOG_VPRINT_ERROR("Device name '%s' too long! Ignoring.",
				ent->d_name);
			continue;
		}
		strcpy(devpath, "/dev/input/");
		strcat(devpath, ent->d_name);

		DEBUG_VPRINT("input-libinput", "Probing device '%s'",
			devpath);

		dev = libinput_path_add_device(li, devpath);
		if (dev == NULL) {
			LOG_VPRINT_ERROR("Could not probe device '%s'",
				devpath);
			continue;
		}

		if (!libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
			LOG_VPRINT_ERROR("Device '%s' does not have KEYBOARD capability! Removing.",
				devpath);
			libinput_path_remove_device(dev);
			continue;
		}

		DEBUG_VPRINT("input-libinput", "Device '%s' (%s) added successfully",
			devpath, libinput_device_get_name(dev));
	}

	closedir(devinput);

	/* launch the event loop on another thread */
	if (pthread_create(&event_loop_thread, NULL, mbi_libinput_event_loop, NULL) != 0) {
		LOG_PRINT_ERROR("pthread_create() failed!");
		return -1;
	}

	return 0;
}


/**
 * Destroy the libinput driver
 */
void
mbi_libinput_destroy(void)
{
	DEBUG_PRINT("input-libinput", "Shutting down libinput driver");
	quit = 1;
	pthread_join(event_loop_thread, NULL);
	libinput_unref(li);
}
