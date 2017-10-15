AUTOMAKE_OPTIONS = subdir-objects

AM_CFLAGS = -std=c99 -Wall -D_GNU_SOURCE \
	-DDATADIR='"$(datadir)"' \
	-DLOCALSTATEDIR='"$(localstatedir)"' \
	@CFLAGS@ \
	@PTHREAD_CFLAGS@ \
	@DIRECTFB_CFLAGS@ \
	@ALSA_CFLAGS@ \
	@PANGOCAIRO_CFLAGS@ \
	@CURL_CFLAGS@ \
	@LIBAVUTIL_CFLAGS@ \
	@LIBAVFORMAT_CFLAGS@ \
	@LIBAVCODEC_CFLAGS@ \
	@LIBAVFILTER_CFLAGS@ \
	@GLIB_CFLAGS@ \
	@GIO_CFLAGS@ \
	@LIBINPUT_CFLAGS@ \
	@SQLITE3_CFLAGS@

AM_LDFLAGS = -lz -lm -lswscale \
	@LIBS@ \
	@PTHREAD_LIBS@ \
	@DIRECTFB_LIBS@ \
	@ALSA_LIBS@ \
	@PANGOCAIRO_LIBS@ \
	@CURL_LIBS@ \
	@LIBAVUTIL_LIBS@ \
	@LIBAVFORMAT_LIBS@ \
	@LIBAVCODEC_LIBS@ \
	@LIBAVFILTER_LIBS@ \
	@GLIB_LIBS@ \
	@GIO_LIBS@ \
	@LIBINPUT_LIBS@ \
	@SQLITE3_LIBS@

bin_PROGRAMS = mediabox

mediabox_SOURCES = \
	lib/queue.c \
	lib/dispatch.c \
	lib/application.c \
	lib/thread.c \
	lib/delegate.c \
	lib/timers.c \
	lib/process.c \
	lib/audio.c \
	lib/settings.c \
	lib/log.c \
	lib/sysinit.c \
	lib/volume.c \
	lib/su.c \
	lib/iface_util.c \
	lib/url_util.c \
	lib/time_util.c \
	lib/pipe_util.c \
	lib/file_util.c \
	lib/string_util.c \
	lib/proc_util.c \
	lib/ui/video.c \
	lib/ui/video-directfb.c \
	lib/ui/listview.c \
	lib/ui/textview.c \
	lib/ui/progressview.c \
	lib/ui/input.c \
	lib/ui/input-libinput.c \
	lib/ui/input-directfb.c \
	lib/ui/input-socket.c \
	lib/ui/input-tcp.c \
	lib/ui/player.c \
	shell.c \
	mainmenu.c \
	downloads.c \
	downloads-backend.c \
	mediasearch.c \
	about.c \
	discovery.c \
	library-backend.c \
	library.c \
	overlay.c \
	main.c

if ENABLE_BLUETOOTH
AM_CFLAGS += @BLUEZ_CFLAGS@
AM_LDFLAGS += @BLUEZ_LIBS@
mediabox_SOURCES += a2dp.c lib/bluetooth.c lib/ui/input-bluetooth.c
endif

if ENABLE_LIBDRM
mediabox_SOURCES += lib/ui/video-drm.c
AM_CFLAGS += @LIBDRM_CFLAGS@
AM_LDFLAGS += @LIBDRM_LIBS@
endif

systemddir = /usr/lib/systemd/system
systemd_DATA = mediabox.service

delugedir = $(datadir)/$(PACKAGE)/deluge
deluge_DATA = res/deluge/core.conf \
	res/deluge/auth

mediatombdir = $(datadir)/$(PACKAGE)/mediatomb
mediatomb_DATA = res/mediatomb/common.js \
	res/mediatomb/config.xml \
	res/mediatomb/import.js \
	res/mediatomb/playlists.js
