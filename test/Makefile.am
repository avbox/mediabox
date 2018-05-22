AUTOMAKE_OPTIONS = subdir-objects

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
	@LIBDVDNAV_LIBS@ \
	@EGL_LIBS@

AVBOX_LIB_SOURCES = ../src/lib/queue.c \
	../src/lib/log.c \
	../src/lib/time_util.c

noinst_PROGRAMS = test-dummy test-primitives
TESTS = test-dummy test-primitives
test_primitives_LDADD =

test_dummy_SOURCES = test-dummy.c
test_primitives_SOURCES = test-primitives.c $(AVBOX_LIB_SOURCES)


if WITH_SYSTEM_LIBTORRENT
AM_LDFLAGS += @LIBTORRENT_LIBS@
AM_CFLAGS += @LIBTORRENT_CFLAGS@
AM_CXXFLAGS += @LIBTORRENT_CFLAGS@
else
AM_CFLAGS += -I../third_party/libtorrent-rasterbar/include
AM_CXXFLAGS += -I../third_party/libtorrent-rasterbar/include
test_primitives_LDADD += ../third_party/libtorrent-rasterbar/src/.libs/libtorrent-rasterbar.la
endif

if WITH_SYSTEM_FFMPEG
AM_LDFLAGS += @LIBSWSCALE_LIBS@ @LIBAVUTIL_LIBS@ @LIBAVFORMAT_LIBS@ @LIBAVCODEC_LIBS@ @LIBAVFILTER_LIBS@
AM_CFLAGS += @LIBSWSCALE_CFLAGS@ @LIBAVUTIL_CFLAGS@ @LIBAVFORMAT_CFLAGS@ @LIBAVCODEC_CFLAGS@ @LIBAVFILTER_CFLAGS@
AM_CXXFLAGS += @LIBSWSCALE_CFLAGS@ @LIBAVUTIL_CFLAGS@ @LIBAVFORMAT_CFLAGS@ @LIBAVCODEC_CFLAGS@ @LIBAVFILTER_CFLAGS@
else
AM_CFLAGS += -I../third_party/ffmpeg
AM_CXXFLAGS += -I../third_party/ffmpeg
AM_LDFLAGS += -ldl -lbz2 -llzma -lz

test_primitives_LDADD += \
	../third_party/ffmpeg/libswscale/libswscale.a \
	../third_party/ffmpeg/libavformat/libavformat.a \
	../third_party/ffmpeg/libavfilter/libavfilter.a \
	../third_party/ffmpeg/libavcodec/libavcodec.a \
	../third_party/ffmpeg/libavutil/libavutil.a \
	../third_party/ffmpeg/libswresample/libswresample.a \
	-ldl -lbz2 -llzma -lz -lm
endif
