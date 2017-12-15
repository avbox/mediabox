################################################################################
#
# libtorrent-rasterbar python bindings
#
################################################################################

LIBTORRENTPYTHON_VERSION = 0.16.17
LIBTORRENTPYTHON_SITE = https://downloads.sourceforge.net/project/libtorrent/libtorrent
LIBTORRENTPYTHON_SOURCE = libtorrent-rasterbar-$(LIBTORRENTPYTHON_VERSION).tar.gz
LIBTORRENTPYTHON_SUBDIR = bindings/python
LIBTORRENTPYTHON_DEPENDENCIES = python host-python host-python-cffi libtorrentrb boost
LIBTORRENTPYTHON_SETUP_TYPE = distutils
LIBTORRENTPYTHON_LICENSE = GPLv2
LIBTORRENTPYTHON_LICENSE_FILES = COPYING

define LIBTORRENTPYTHON_PATCH_LIBDIR
	sed -i -e "s|/opt/local/lib|$(TARGET_DIR)/usr/lib|g" \
		$(BUILD_DIR)/libtorrentpython-0.16.17/bindings/python/setup.py
	sed -i -e "s|-mt||g" \
		$(BUILD_DIR)/libtorrentpython-0.16.17/bindings/python/setup.py

endef

LIBTORRENTPYTHON_POST_PATCH_HOOKS += LIBTORRENTPYTHON_PATCH_LIBDIR

$(eval $(python-package))
