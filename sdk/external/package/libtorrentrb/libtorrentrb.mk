################################################################################
#
# libtorrentrb
#
################################################################################

LIBTORRENTRB_VERSION = 1.1.6
LIBTORRENTRB_SITE = https://github.com/arvidn/libtorrent/releases/download/libtorrent-1_1_6
LIBTORRENTRB_SOURCE = libtorrent-rasterbar-$(LIBTORRENTRB_VERSION).tar.gz
LIBTORRENTRB_DEPENDENCIES = host-pkgconf zlib boost \
	$(if $(BR2_PACKAGE_OPENSSL),openssl)
LIBTORRENTRB_LICENSE = GPLv2
LIBTORRENTRB_INSTALL_STAGING = YES
LIBTORRENTRB_LICENSE_FILES = COPYING

LIBTORRENTRB_CONF_ENV = CXXFLAGS="-std=c++11"
LIBTORRENTRB_CONF_OPTS += \
	--with-boost-libdir=$(TARGET_DIR)/usr/lib


define LIBTORRENTRB_COPY_HEADERS
	cp -rv $(TARGET_DIR)/usr/include/libtorrent $(HOST_DIR)/usr/$(GNU_TARGET_NAME)/sysroot/usr/include
endef

#LIBTORRENTRB_POST_INSTALL_TARGET_HOOKS += LIBTORRENTRB_COPY_HEADERS

$(eval $(autotools-package))
