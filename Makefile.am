AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4

ALL_DEPS =
CLEAN_DEPS =


if !WITH_SYSTEM_FFMPEG
ALL_DEPS += ffmpeg-all
CLEAN_DEPS += ffmpeg-clean
endif

if !WITH_SYSTEM_LIBTORRENT
ALL_DEPS += libtorrent-all
CLEAN_DEPS += libtorrent-clean
endif

if !WITH_SYSTEM_LIBWEBSOCKETS
ALL_DEPS += libwebsockets-all
CLEAN_DEPS += libwebsockets-clean
endif


#
# rules for bundled libraries
#
ffmpeg-all:
	cd third_party/ffmpeg && $(MAKE)
ffmpeg-clean:
	cd third_party/ffmpeg && $(MAKE) clean
libtorrent-all:
	cd third_party/libtorrent-rasterbar && $(MAKE)
libtorrent-clean:
	cd third_party/libtorrent-rasterbar && $(MAKE) clean
libwebsockets-all:
	cd third_party/libwebsockets && $(MAKE)
libwebsockets-clean:
	cd third_party/libwebsockets && $(MAKE) clean


#
# rules for unit tests
#
tests-all:
	cd test && $(MAKE)
tests-clean:
	cd test && $(MAKE) clean


#
# default rules
#
all-local: $(ALL_DEPS)
	cd src && $(MAKE)
clean-local: $(CLEAN_DEPS) tests-clean
	cd src && $(MAKE) clean
check-local: tests-all
	cd test && $(MAKE) check
install-exec-local:
	cd src && $(MAKE) install
