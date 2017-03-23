################################################################################
#
# libtorrent-rasterbar
#
################################################################################

LIBTORRENT_RASTERBAR_VERSION = 0.16.17
LIBTORRENT_RASTERBAR_SITE = https://downloads.sourceforge.net/project/libtorrent/libtorrent
LIBTORRENT_RASTERBAR_SOURCE = libtorrent-rasterbar-$(LIBTORRENT_RASTERBAR_VERSION).tar.gz
LIBTORRENT_RASTERBAR_DEPENDENCIES = host-pkgconf zlib boost \
	$(if $(BR2_PACKAGE_OPENSSL),openssl)
LIBTORRENT_RASTERBAR_LICENSE = GPLv2
LIBTORRENT_RASTERBAR_LICENSE_FILES = COPYING

LIBTORRENT_RASTERBAR_CONF_OPTS += \
	--with-boost-libdir=$(TARGET_DIR)/usr/lib

ifeq ($(BR2_PACKAGE_LIBTORRENT_RASTERBAR_PYTHON),y)
LIBTORRENT_RASTERBAR_CONF_OPTS += --enable-python-binding \
	--with-boost-python=$(TARGET_DIR)/us/lib/boost_python
LIBTORRENT_RASTERBAR_MAKE_ENV += CC="$(HOST_DIR)/usr/bin/x86_64-mediabox-linux-uclibc-gcc"
LIBTORRENT_RASTERBAR_MAKE_ENV += CXX="$(HOST_DIR)/usr/bin/x86_64-mediabox-linux-uclibc-g++"
endif

$(eval $(autotools-package))
