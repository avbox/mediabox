################################################################################
#
# libtorrentrb
#
################################################################################

LIBTORRENTRB_VERSION = 0.16.17
LIBTORRENTRB_SITE = https://downloads.sourceforge.net/project/libtorrent/libtorrent
LIBTORRENTRB_SOURCE = libtorrent-rasterbar-$(LIBTORRENTRB_VERSION).tar.gz
LIBTORRENTRB_DEPENDENCIES = host-pkgconf zlib boost \
	$(if $(BR2_PACKAGE_OPENSSL),openssl)
LIBTORRENTRB_LICENSE = GPLv2
LIBTORRENTRB_LICENSE_FILES = COPYING

LIBTORRENTRB_CONF_OPTS += \
	--with-boost-libdir=$(TARGET_DIR)/usr/lib

ifeq ($(BR2_PACKAGE_LIBTORRENTRB_PYTHON),y)
LIBTORRENTRB_DEPENDENCIES += host-python host-python-cffi python
LIBTORRENTRB_CONF_OPTS += --enable-python-binding \
	--with-boost-python=$(TARGET_DIR)/usr/lib/boost_python
LIBTORRENTRB_MAKE_ENV += CC="$(HOST_DIR)/usr/bin/x86_64-mediabox-linux-uclibc-gcc"
LIBTORRENTRB_MAKE_ENV += CXX="$(HOST_DIR)/usr/bin/x86_64-mediabox-linux-uclibc-g++"
endif

$(eval $(autotools-package))
