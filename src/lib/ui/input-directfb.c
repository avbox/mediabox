#include <stdlib.h>
#include <pthread.h>
#include <directfb.h>

#define LOG_MODULE "input"

#include "input.h"
#include "../debug.h"
#include "../log.h"


extern IDirectFB *dfb;
static DFBInputDeviceID keyboard_device_id = -1;
static IDirectFBInputDevice *keyboard_device = NULL;
static IDirectFBEventBuffer *events = NULL;
static pthread_t event_loop_thread;
static int quit = 0;


#define CASE_KEYBOARD(x) \
	case DIKS_SMALL_ ## x: \
	case DIKS_CAPITAL_ ## x: \
		avbox_input_sendevent(MBI_EVENT_KBD_ ## x); \
		break;



/**
 * Runs the directfb input driver event loop
 */
static void*
mbi_directfb_event_loop(void *arg)
{
	DFBInputEvent e;
	DFBResult ret;

	(void) arg;

	DEBUG_PRINT("input-directfb", "Running DirectFB input loop");
	DEBUG_SET_THREAD_NAME("input-dfb");

	while (!quit) {
		if (events->WaitForEvent(events) == DFB_OK) {
			if ((ret = events->GetEvent(events, DFB_EVENT(&e))) != DFB_OK) {
				LOG_VPRINT_ERROR("GetEvent() returned %i. Aborting", ret);
				abort();
			}
			switch (e.type) {
			case DIET_KEYPRESS:
				switch (e.key_symbol) {
				case DIKS_ESCAPE:       avbox_input_sendevent(MBI_EVENT_BACK); break;
				case DIKS_RETURN:       avbox_input_sendevent(MBI_EVENT_ENTER); break;
				case DIKS_SHIFT:        avbox_input_sendevent(MBI_EVENT_MENU); break;
				case DIKS_CURSOR_UP:    avbox_input_sendevent(MBI_EVENT_ARROW_UP); break;
				case DIKS_CURSOR_DOWN:  avbox_input_sendevent(MBI_EVENT_ARROW_DOWN); break;
				case DIKS_CURSOR_LEFT:  avbox_input_sendevent(MBI_EVENT_ARROW_LEFT); break;
				case DIKS_CURSOR_RIGHT: avbox_input_sendevent(MBI_EVENT_ARROW_RIGHT); break;
				case DIKS_BACKSPACE:    avbox_input_sendevent(MBI_EVENT_CLEAR); break;
				case DIKS_SPACE:        avbox_input_sendevent(MBI_EVENT_KBD_SPACE); break;

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

	DEBUG_PRINT("input-directfb", "Exiting DirectFB event loop");
	return NULL;
}


/**
 * Callback function used by directfb to return enumerated devices
 */
DFBEnumerationResult
enum_devices_callback(
  	DFBInputDeviceID device_id,
  	DFBInputDeviceDescription desc,
  	void *callbackdata)
{
	if (keyboard_device_id == -1 && desc.type & DIDTF_KEYBOARD) {
		keyboard_device_id = device_id;	
		DEBUG_VPRINT("input-directfb", "Discovered device: %s (id=%i)",
			desc.name, device_id);
	}
	return 0;
}


/**
 * Initialize the directfb input driver
 */
int
mbi_directfb_init(void)
{
	DEBUG_PRINT("input-directfb", "Initializing DirectFB input driver");

	/* make sure directfb has been initialized */
	if (dfb == NULL) {
		LOG_PRINT_ERROR("DirectFB not initialized!");
		return -1;
	}

	/* find a keyboard device */
	if (dfb->EnumInputDevices(dfb, enum_devices_callback, NULL) != DFB_OK) {
		LOG_PRINT_ERROR("EnumInputDevices() failed");
		return -1;
	}
	if (keyboard_device_id == -1) {
		LOG_PRINT_ERROR("No input devices found!");
		return -1;
	}
	if (dfb->GetInputDevice(dfb, keyboard_device_id, &keyboard_device) != DFB_OK) {
		LOG_PRINT_ERROR("Could not get device interface!");
		return -1;
	}

	/* create events buffer */
	if (keyboard_device->CreateEventBuffer(keyboard_device, &events) != DFB_OK) {
		LOG_PRINT_ERROR("CreateEventBuffer() failed!");
		return -1;
	}

	/* launch the event loop on another thread */
	if (pthread_create(&event_loop_thread, NULL, mbi_directfb_event_loop, NULL) != 0) {
		LOG_PRINT_ERROR("pthread_create() failed!");
		return -1;
	}
	return 0;
}


/**
 * Destroy the directfb input driver.
 */
void
mbi_directfb_destroy(void)
{
	DEBUG_PRINT("input-directfb", "Shutting down DirectFB input driver");

	if (dfb != NULL) {
		quit = 1;
		events->WakeUp(events);
		pthread_join(event_loop_thread, NULL);
		events->Release(events);
		if (keyboard_device != NULL) {
			keyboard_device->Release(keyboard_device);
		}
	}
}
