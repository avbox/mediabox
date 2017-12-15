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

$(eval $(autotools-package))
