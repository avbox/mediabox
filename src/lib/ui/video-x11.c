#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <GL/glx.h>

#define LOG_MODULE "video-x11"

#include <libavbox/avbox.h>


/* X11 driver */
static int initialized = 0;
static Display *xdisplay;
static Window xwindow;
static Colormap xcolormap;
static GLXContext xgl;


static void
swap_buffers(void)
{
	glXSwapBuffers(xdisplay, xwindow);
}


/**
 * Initialize the X11 driver.
 */
static struct mbv_surface *
init(struct mbv_drv_funcs * const driver, int argc, char **argv, int * const w, int * const h)
{
	GLint att[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };

	struct mbv_surface *surface = NULL;

	Window root_window;
	XVisualInfo *vi;
	XWindowAttributes gwa;
	XSetWindowAttributes swa;
	Atom wm_state, wm_state_fullscreen;
	char cursor_data = 0;
	Pixmap blank;
	XColor dummy;
	Cursor cursor;

	if ((xdisplay = XOpenDisplay(NULL)) == NULL) {
		LOG_PRINT_ERROR("Could not open display!");
		goto end;
	}

	root_window = DefaultRootWindow(xdisplay);
	XGetWindowAttributes(xdisplay, root_window, &gwa);

	*w = gwa.width;
	*h = gwa.height;

	if ((vi = glXChooseVisual(xdisplay, 0, att)) == NULL) {
		LOG_PRINT_ERROR("glXChooseVisual() failed!");
		XCloseDisplay(xdisplay);
		goto end;
	}

	/* create X11 window */
	xcolormap = XCreateColormap(xdisplay, root_window, vi->visual, AllocNone);
	swa.colormap = xcolormap;
	swa.event_mask = 0;
	xwindow = XCreateWindow(xdisplay, root_window, 0, 0, gwa.width, gwa.height, 0, vi->depth,
		InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
	XMapWindow(xdisplay, xwindow);

	/* send the fullscreen wm hint */
	wm_state = XInternAtom(xdisplay, "_NET_WM_STATE", 1);
	wm_state_fullscreen = XInternAtom(xdisplay, "_NET_WM_STATE_FULLSCREEN", 1);
	if (wm_state != None && wm_state_fullscreen != None) {
		XClientMessageEvent msg;
		msg.type = ClientMessage;
		msg.window = xwindow;
		msg.message_type = wm_state;
		msg.format = 32;
		msg.data.l[0] = 1; /* _NET_WM_STATE_ADD */
		msg.data.l[1] = wm_state_fullscreen;
		msg.data.l[2] = 0;
		msg.data.l[3] = 0;
		msg.data.l[4] = 0;
		XSendEvent(xdisplay, root_window, False,
			SubstructureRedirectMask | SubstructureNotifyMask,
			(XEvent*) &msg);
	}

	/* hide cursor */
	if ((blank = XCreateBitmapFromData(xdisplay, xwindow, &cursor_data, 1, 1)) == None) {
		LOG_PRINT_ERROR("Could not create invisible cursor pixmap!");
	} else {
		if ((cursor = XCreatePixmapCursor(xdisplay, blank, blank, &dummy, &dummy, 0, 0)) == None) {
			LOG_PRINT_ERROR("Could not create invisible cursor!");
		} else {
			XDefineCursor(xdisplay, xwindow, cursor);
		}
		XFreePixmap(xdisplay, blank);
	}

	/* create GL context */
	if ((xgl = glXCreateContext(xdisplay, vi, NULL, GL_TRUE)) == NULL) {
		LOG_PRINT_ERROR("Could not create GL context!");
		XDestroyWindow(xdisplay, xwindow);
		XFreeColormap(xdisplay, xcolormap);
		XCloseDisplay(xdisplay);
		goto end;
	} else {
		glXMakeCurrent(xdisplay, xwindow, xgl);
	}

	DEBUG_VPRINT(LOG_MODULE, "X11 window created (w=%d,h=%d)",
		gwa.width, gwa.height);

	/* initialize the GL driver */
	if ((surface = avbox_video_glinit(
		driver, gwa.width, gwa.height,
		swap_buffers)) == NULL) {
		LOG_PRINT_ERROR("GL setup failed");
		XDestroyWindow(xdisplay, xwindow);
		XFreeColormap(xdisplay, xcolormap);
		XCloseDisplay(xdisplay);
		goto end;
	} else {
		DEBUG_PRINT(LOG_MODULE, "GL Driver Initialized");
	}

end:
	return surface;
}


static void
shutdown(void)
{
	if (initialized) {
		glXMakeCurrent(xdisplay, None, NULL);
		glXDestroyContext(xdisplay, xgl);
		XDestroyWindow(xdisplay, xwindow);
		XFreeColormap(xdisplay, xcolormap);
		XCloseDisplay(xdisplay);
	}
}


INTERNAL void
avbox_video_x11_initft(struct mbv_drv_funcs * const funcs)
{
	memset(funcs, 0, sizeof(struct mbv_drv_funcs));
	funcs->init = &init;
	funcs->shutdown = &shutdown;
}
