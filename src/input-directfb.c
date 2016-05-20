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

static struct mbi *instance;

static void*
mbi_directfb_event_loop(void *arg)
{
	DFBWindowEvent e;
	DFBResult ret;

	fprintf(stderr, "Running directfb event loop\n");

	while (!quit) {
		if ((ret = events->GetEvent(events, DFB_EVENT(&e))) == DFB_OK) {
			switch (e.type) {
			case DWET_KEYDOWN:
				switch (e.key_symbol) {
				case DIKS_ESCAPE:
					fprintf(stderr, "ESC Pressed\n");
					break;
				case DIKS_RETURN:
					fprintf(stderr, "RETURN Pressed\n");
					break;
				//default:
				//	break;
				}
				break;
			default:
				break;
			}
		} else {
			sleep(1);
		}
	}
	fprintf(stderr, "Exiting directfb event loop\n");
	return NULL;
}

DFBEnumerationResult enum_devices_callback(
  	DFBInputDeviceID device_id,
  	DFBInputDeviceDescription desc,
  	void *callbackdata)
{
	if (keyboard_device_id == -1 && desc.type & DIDTF_KEYBOARD) {
		keyboard_device_id = device_id;	
		fprintf(stderr, "Device id: %i\n", device_id);
		fprintf(stderr, "Device name: %s\n", desc.name);
	}
	return 0;
}

int
mbi_directfb_init(struct mbi *inst)
{

	instance = inst;

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


	/* send test event */
	mbi_event_send(inst, MBI_EVENT_PLAY);

	return 0;
}

void
mbi_directfb_destroy()
{
	quit = 1;
	pthread_join(event_loop_thread, NULL);
	events->Release(events);
}

