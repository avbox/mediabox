AUTOMAKE_OPTIONS = subdir-objects

AM_SHARED_FLAGS = \
	-DDEBUG_MEMORY_POOLS \
	-DDATADIR="$(datadir)" \
	-DLOCALSTATEDIR="$(localstatedir)" \
	@CFLAGS@ \
	@PTHREAD_CFLAGS@ \
	@ALSA_CFLAGS@ \
	@PANGOCAIRO_CFLAGS@ \
	@CURL_CFLAGS@ \
	@GLIB_CFLAGS@ \
	@GIO_CFLAGS@ \
	@SQLITE3_CFLAGS@ \
	@LIBUDEV_CFLAGS@ \
	@LIBDVDCSS_CFLAGS@ \
	@LIBDVDNAV_CFLAGS@ \
	@EGL_CFLAGS@

AM_CFLAGS = -std=c99 -Wall -D_GNU_SOURCE $(AM_SHARED_FLAGS)
AM_CXXFLAGS = -std=c++11 -Wall $(AM_SHARED_FLAGS)

AM_LDFLAGS = -lz -lm \
	@LIBS@ \
	@PTHREAD_LIBS@ \
	@ALSA_LIBS@ \
	@PANGOCAIRO_LIBS@ \
	@CURL_LIBS@ \
	@GLIB_LIBS@ \
	@GIO_LIBS@ \
	@SQLITE3_LIBS@ \
	@LIBUDEV_LIBS@ \
	@LIBDVDCSS_LIBS@ \
	@LIBDVDNAV_LIBS@ \
	@EGL_LIBS@

bin_PROGRAMS = mediabox

mediabox_LDADD =
mediabox_CFLAGS = $(AM_CFLAGS)
mediabox_CXXFLAGS = $(AM_CXXFLAGS)
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
	lib/ffmpeg_util.c \
	lib/db_util.c \
	lib/stream.c \
	lib/stopwatch.c \
	lib/ui/video.c \
	lib/ui/video-software.c \
	lib/ui/player.c \
	lib/ui/listview.c \
	lib/ui/textview.c \
	lib/ui/progressview.c \
	lib/ui/input.c \
	lib/ui/input-socket.c \
	lib/ui/input-tcp.c \
	shell.c \
	mainmenu.c \
	downloads.c \
	downloads-backend.c \
	about.c \
	discovery.c \
	library.c \
	browser.c \
	overlay.c \
	main.c

if ENABLE_LIBINPUT
AM_CFLAGS += @LIBINPUT_CFLAGS@
AM_LDFLAGS += @LIBINPUT_LIBS@
mediabox_SOURCES += lib/ui/input-libinput.c
endif

if ENABLE_DIRECTFB
AM_CFLAGS += @DIRECTFB_CFLAGS@
AM_LDFLAGS += @DIRECTFB_LIBS@
mediabox_SOURCES += lib/ui/video-directfb.c lib/ui/input-directfb.c
endif

if ENABLE_BLUETOOTH
AM_CFLAGS += @BLUEZ_CFLAGS@
AM_LDFLAGS += @BLUEZ_LIBS@
mediabox_SOURCES += a2dp.c lib/bluetooth.c lib/ui/input-bluetooth.c
endif

if ENABLE_LIBDRM
mediabox_SOURCES += lib/ui/video-drm.c
AM_CFLAGS += @LIBDRM_CFLAGS@ @EGL_CFLAGS@ @GBM_CFLAGS@
AM_LDFLAGS += @LIBDRM_LIBS@ @EGL_LIBS@ @GBM_LIBS@
endif

if ENABLE_DVD
mediabox_SOURCES += lib/dvdio.c
endif

if ENABLE_X11
mediabox_SOURCES += lib/ui/video-x11.c
endif

if ENABLE_VC4
noinst_LIBRARIES = libvc4.a
libvc4_a_SOURCES = lib/ui/video-vc4.c lib/ui/mmaldecode.c
libvc4_a_CFLAGS = $(AM_CFLAGS) -std=gnu89
mediabox_LDADD += $(AM_LDFLAGS) libvc4.a
endif

if WITH_SYSTEM_LIBTORRENT
AM_LDFLAGS += @LIBTORRENT_LIBS@
AM_CXXFLAGS += @LIBTORRENT_CFLAGS@
else
mediabox_CFLAGS += -I../third_party/libtorrent-rasterbar/include
mediabox_CXXFLAGS += -I../third_party/libtorrent-rasterbar/include
mediabox_LDADD += ../third_party/libtorrent-rasterbar/src/.libs/libtorrent-rasterbar.la
endif

mediabox_SOURCES += lib/torrent_stream.cpp lib/torrent_in.c

if ENABLE_OPENGL
mediabox_SOURCES += lib/ui/video-opengl.c
endif

if ENABLE_WEBREMOTE
mediabox_SOURCES += lib/ui/input-web.c
if WITH_SYSTEM_LIBWEBSOCKETS
AM_CXXFLAGS += @LIBWEBSOCKETS_CFLAGS@
AM_LDFLAGS += @LIBWEBSOCKETS_LIBS@
else
mediabox_CFLAGS += -I../third_party/libwebsockets/include
mediabox_LDADD += ../third_party/libwebsockets/lib/libwebsockets.a
endif
endif

if WITH_SYSTEM_FFMPEG
AM_LDFLAGS += @LIBSWSCALE_LIBS@ @LIBAVUTIL_LIBS@ @LIBAVFORMAT_LIBS@ @LIBAVCODEC_LIBS@ @LIBAVFILTER_LIBS@
AM_CXXFLAGS += @LIBSWSCALE_CFLAGS@ @LIBAVUTIL_CFLAGS@ @LIBAVFORMAT_CFLAGS@ @LIBAVCODEC_CFLAGS@ @LIBAVFILTER_CFLAGS@
else
mediabox_LDADD += \
	../third_party/ffmpeg/libswscale/libswscale.a \
	../third_party/ffmpeg/libavformat/libavformat.a \
	../third_party/ffmpeg/libavfilter/libavfilter.a \
	../third_party/ffmpeg/libavcodec/libavcodec.a \
	../third_party/ffmpeg/libavutil/libavutil.a \
	../third_party/ffmpeg/libswresample/libswresample.a \
	-ldl -lbz2 -llzma -lz
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

if ENABLE_WEBREMOTE
webremotedir = $(datadir)/$(PACKAGE)
webremote_DATA = res/mediabox/webremote.html
endif
