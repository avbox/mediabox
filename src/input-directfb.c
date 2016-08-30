#include <stdlib.h>
#include <pthread.h>
#include <directfb.h>

#include "input.h"

extern IDirectFB *dfb;
static DFBInputDeviceID keyboard_device_id = -1;
static IDirectFBInputDevice *keyboard_device = NULL;
static IDirectFBEventBuffer *events = NULL;
static pthread_t event_loop_thread;
static int quit = 0;

#define CASE_KEYBOARD(x) \
	case DIKS_SMALL_ ## x: \
	case DIKS_CAPITAL_ ## x: \
		mbi_event_send(MBI_EVENT_KBD_ ## x); \
		break;



/**
 * mbi_directfb_event_loop() -- Runs the directfb input driver event loop
 */
static void*
mbi_directfb_event_loop(void *arg)
{
	DFBInputEvent e;
	DFBResult ret;

	(void) arg;

	fprintf(stderr, "mbi: Running directfb input event loop\n");

	while (!quit) {
		if (events->WaitForEvent(events) == DFB_OK) {
			if ((ret = events->GetEvent(events, DFB_EVENT(&e))) != DFB_OK) {
				fprintf(stderr, "mbi: GetEvents() returned %i\n", ret);
				abort();
			}
			switch (e.type) {
			case DIET_KEYPRESS:
				switch (e.key_symbol) {
				case DIKS_ESCAPE:       mbi_event_send(MBI_EVENT_BACK); break;
				case DIKS_RETURN:       mbi_event_send(MBI_EVENT_ENTER); break;
				case DIKS_SHIFT:        mbi_event_send(MBI_EVENT_MENU); break;
				case DIKS_CURSOR_UP:    mbi_event_send(MBI_EVENT_ARROW_UP); break;
				case DIKS_CURSOR_DOWN:  mbi_event_send(MBI_EVENT_ARROW_DOWN); break;
				case DIKS_CURSOR_LEFT:  mbi_event_send(MBI_EVENT_ARROW_LEFT); break;
				case DIKS_CURSOR_RIGHT: mbi_event_send(MBI_EVENT_ARROW_RIGHT); break;
				case DIKS_BACKSPACE:    mbi_event_send(MBI_EVENT_CLEAR); break;
				case DIKS_SPACE:        mbi_event_send(MBI_EVENT_KBD_SPACE); break;

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

				default: break;
				}
				break;
			default:
				/* fprintf(stderr, "Event type %i\n", e.type); */
				break;
			}
		}
	}

	fprintf(stderr, "mbi: Exiting directfb input event loop\n");
	return NULL;
}

/**
 * enum_devices_callback() -- Callback function used by directfb
 * to return enumerated devices
 */
DFBEnumerationResult
enum_devices_callback(
  	DFBInputDeviceID device_id,
  	DFBInputDeviceDescription desc,
  	void *callbackdata)
{
	if (keyboard_device_id == -1 && desc.type & DIDTF_KEYBOARD) {
		keyboard_device_id = device_id;	
		fprintf(stderr, "input-dfb: Discovered device: %s (id=%i)\n",
			desc.name, device_id);
	}
	return 0;
}


/**
 * mbi_directfb_init() -- Initialize the directfb input driver
 */
int
mbi_directfb_init(void)
{
	/* make sure directfb has been initialized */
	if (dfb == NULL) {
		fprintf(stderr, "mbi_directfb_init() failed!\n");
		return -1;
	}

	/* find a keyboard device */
	if (dfb->EnumInputDevices(dfb, enum_devices_callback, NULL) != DFB_OK) {
		fprintf(stderr, "dfb->EnumInputDevices() failed\n");
		return -1;
	}
	if (keyboard_device_id == -1) {
		fprintf(stderr, "No input devices found\n");
		return -1;
	}
	if (dfb->GetInputDevice(dfb, keyboard_device_id, &keyboard_device) != DFB_OK) {
		fprintf(stderr, "Could not get device interface\n");
		return -1;
	}

	/* create events buffer */
	if (keyboard_device->CreateEventBuffer(keyboard_device, &events) != DFB_OK) {
		fprintf(stderr, "dfb->CreateEventBuffer() failed\n");
		return -1;
	}

	/* launch the event loop on another thread */
	if (pthread_create(&event_loop_thread, NULL, mbi_directfb_event_loop, NULL) != 0) {
		fprintf(stderr, "pthread_create() failed\n");
		return -1;
	}
	return 0;
}

/**
 * mbi_directfb_destroy() -- Destroy the directfb input driver
 */
void
mbi_directfb_destroy(void)
{
	fprintf(stderr, "mbi: Destroying DirectFB input system\n");
	quit = 1;
	events->WakeUp(events);
	pthread_join(event_loop_thread, NULL);
	events->Release(events);
	if (keyboard_device != NULL) {
		keyboard_device->Release(keyboard_device);
	}
}

