
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
	@GIO_CFLAGS@

AM_LDFLAGS = -lz -lm \
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
	@GIO_LIBS@

bin_PROGRAMS = mediabox

mediabox_SOURCES = \
	main.c \
	video.c \
	video-directfb.c \
	audio.c \
	input.c \
	input-directfb.c \
	input-socket.c \
	input-tcp.c \
	pipe_util.c \
	player.c \
	shell.c \
	su.c \
	ui-textview.c \
	ui-menu.c \
	ui-progressbar.c \
	mainmenu.c \
	downloads.c \
	downloads-backend.c \
	mediasearch.c \
	url_util.c \
	time_util.c \
	file_util.c \
	string_util.c \
	about.c \
	announce.c \
	iface_util.c \
	library-backend.c \
	library.c \
	timers.c \
	alsa-volume.c \
	process.c \
	sysinit.c \
	proc_util.c

if ENABLE_BLUETOOTH
AM_CFLAGS += @BLUEZ_CFLAGS@
AM_LDFLAGS += @BLUEZ_LIBS@
mediabox_SOURCES += input-bluetooth.c
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
