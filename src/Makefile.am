AUTOMAKE_OPTIONS = subdir-objects


noinst_LIBRARIES =
noinst_LTLIBRARIES = libavbox.la
bin_PROGRAMS = mediabox


AM_SHARED_FLAGS = \
	-DDEBUG_MEMORY_POOLS \
	-DDATADIR="$(datadir)" \
	-DLOCALSTATEDIR="$(localstatedir)" \
	-I../include \
	@CFLAGS@ \
	@PTHREAD_CFLAGS@ \
	@ALSA_CFLAGS@ \
	@PANGOCAIRO_CFLAGS@ \
	@CURL_CFLAGS@ \
	@GLIB_CFLAGS@ \
	@GIO_CFLAGS@ \
	@SQLITE3_CFLAGS@ \
	@LIBUDEV_CFLAGS@ \
	@LIBDVDNAV_CFLAGS@ \
	@EGL_CFLAGS@


AM_CFLAGS = -std=c99 -Wall -D_GNU_SOURCE $(AM_SHARED_FLAGS)
AM_CXXFLAGS = -std=c++11 -Wall $(AM_SHARED_FLAGS)
AM_LDFLAGS = -lz -lm -lstdc++ \
	@LIBS@ \
	@PTHREAD_LIBS@ \
	@ALSA_LIBS@ \
	@PANGOCAIRO_LIBS@ \
	@CURL_LIBS@ \
	@GLIB_LIBS@ \
	@GIO_LIBS@ \
	@SQLITE3_LIBS@ \
	@LIBUDEV_LIBS@ \
	@LIBDVDNAV_LIBS@ \
	@EGL_LIBS@


#
# libavbox library
#
libavbox_la_LIBADD =
libavbox_la_CFLAGS = $(AM_CFLAGS)
libavbox_la_CXXFLAGS = $(AM_CXXFLAGS)
libavbox_la_LDFLAGS = $(AM_LDFLAGS)
libavbox_la_SOURCES = \
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
	lib/torrent_stream.cpp \
	lib/torrent_in.c

#
# mediabox binary
#
mediabox_LDADD = libavbox.la
mediabox_CFLAGS = $(AM_CFLAGS)
mediabox_CXXFLAGS = $(AM_CXXFLAGS)
mediabox_LDFLAGS = $(AM_LDFLAGS)
mediabox_SOURCES = \
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

# this is to force autotools to use the C++ compiler for linking
# See https://www.gnu.org/software/automake/manual/html_node/Libtool-Convenience-Libraries.html
nodist_EXTRA_mediabox_SOURCES = dummy.cpp

if ENABLE_LIBINPUT
libavbox_la_CFLAGS += @LIBINPUT_CFLAGS@
libavbox_la_LDFLAGS += @LIBINPUT_LIBS@
libavbox_la_SOURCES += lib/ui/input-libinput.c
endif

if ENABLE_DIRECTFB
libavbox_la_CFLAGS += @DIRECTFB_CFLAGS@
libavbox_la_LDFLAGS += @DIRECTFB_LIBS@
libavbox_la_SOURCES += lib/ui/video-directfb.c lib/ui/input-directfb.c
endif

if ENABLE_BLUETOOTH
libavbox_la_CFLAGS += @BLUEZ_CFLAGS@
libavbox_la_LDFLAGS += @BLUEZ_LIBS@
libavbox_la_SOURCES += lib/bluetooth.c lib/ui/input-bluetooth.c
if ENABLE_A2DP
mediabox_SOURCES += a2dp.c
endif
endif

if ENABLE_LIBDRM
libavbox_la_SOURCES += lib/ui/video-drm.c
libavbox_la_LDFLAGS += @LIBDRM_LIBS@ @EGL_LIBS@ @GBM_LIBS@
libavbox_la_CFLAGS += @LIBDRM_CFLAGS@ @EGL_CFLAGS@ @GBM_CFLAGS@
endif

if ENABLE_DVD
libavbox_la_SOURCES += lib/dvdio.c
endif

if ENABLE_X11
libavbox_la_SOURCES += lib/ui/video-x11.c
endif

if ENABLE_VC4
noinst_LIBRARIES += libvc4.a
libvc4_a_SOURCES = lib/ui/video-vc4.c lib/ui/mmaldecode.c
libvc4_a_CFLAGS = $(AM_CFLAGS) -std=gnu89
mediabox_LDADD += $(AM_LDFLAGS) libvc4.a
endif

if WITH_SYSTEM_LIBTORRENT
libavbox_la_LDFLAGS += @LIBTORRENT_LIBS@
libavbox_la_CXXFLAGS += @LIBTORRENT_CFLAGS@
else
libavbox_la_CFLAGS += -I../third_party/libtorrent-rasterbar/include
libavbox_la_CXXFLAGS += -I../third_party/libtorrent-rasterbar/include
libavbox_la_LIBADD += ../third_party/libtorrent-rasterbar/src/.libs/libtorrent-rasterbar.la
endif


if ENABLE_OPENGL
libavbox_la_SOURCES += lib/ui/video-opengl.c
endif

if ENABLE_WEBREMOTE
libavbox_la_SOURCES += lib/ui/input-web.c
if WITH_SYSTEM_LIBWEBSOCKETS
libavbox_la_CXXFLAGS += @LIBWEBSOCKETS_CFLAGS@
libavbox_la_LDFLAGS += @LIBWEBSOCKETS_LIBS@
else
mediabox_CFLAGS += -I../third_party/libwebsockets/include
libavbox_la_CFLAGS += -I../third_party/libwebsockets/include
mediabox_LDADD += ../third_party/libwebsockets/lib/libwebsockets.a
endif
endif

if WITH_SYSTEM_FFMPEG
libavbox_la_LDFLAGS += @LIBSWSCALE_LIBS@ @LIBAVUTIL_LIBS@ @LIBAVFORMAT_LIBS@ @LIBAVCODEC_LIBS@ @LIBAVFILTER_LIBS@
libavbox_la_CXXFLAGS += @LIBSWSCALE_CFLAGS@ @LIBAVUTIL_CFLAGS@ @LIBAVFORMAT_CFLAGS@ @LIBAVCODEC_CFLAGS@ @LIBAVFILTER_CFLAGS@
else
mediabox_CFLAGS += -I../third_party/ffmpeg
mediabox_CXXFLAGS += -I../third_party/ffmpeg
mediabox_LDFLAGS += -ldl -lbz2 -llzma -lz

libavbox_la_CFLAGS += -I../third_party/ffmpeg
libavbox_la_CXXFLAGS += -I../third_party/ffmpeg
mediabox_LDADD += \
	../third_party/ffmpeg/libswscale/libswscale.a \
	../third_party/ffmpeg/libavformat/libavformat.a \
	../third_party/ffmpeg/libavfilter/libavfilter.a \
	../third_party/ffmpeg/libavcodec/libavcodec.a \
	../third_party/ffmpeg/libavutil/libavutil.a \
	../third_party/ffmpeg/libswresample/libswresample.a \
	-ldl -lbz2 -llzma -lz -lm
endif

systemddir = /usr/lib/systemd/system
systemd_DATA = mediabox.service

mediatombdir = $(datadir)/$(PACKAGE)/mediatomb
mediatomb_DATA = res/mediatomb/common.js \
	res/mediatomb/config.xml \
	res/mediatomb/import.js \
	res/mediatomb/playlists.js

if ENABLE_WEBREMOTE
webremotedir = $(datadir)/$(PACKAGE)
webremote_DATA = res/mediabox/webremote.html
endif
