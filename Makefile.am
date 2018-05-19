AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4
SUBDIRS =

if !WITH_SYSTEM_LIBTORRENT
SUBDIRS += third_party/libtorrent-rasterbar
endif
if !WITH_SYSTEM_FFMPEG
SUBDIRS += third_party/ffmpeg
endif

SUBDIRS += src
