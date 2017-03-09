################################################################################
#
# mediatomb
#
################################################################################

MEDIATOMB_VERSION = 0.12.1
MEDIATOMB_SITE = https://downloads.sourceforge.net/project/mediatomb/MediaTomb/0.12.1
MEDIATOMB_SOURCE = mediatomb-${MEDIATOMB_VERSION}.tar.gz
MEDIATOMB_LICENSE = GPLv2
MEDIATOMB_LICENSE_FILES = COPYING
MEDIATOMB_INSTALL_STAGING = YES
MEDIATOMB_AUTORECONF = YES
MEDIATOMB_CONF_OPTS = CXXFLAGS="-fpermissive $(TARGET_CXXFLAGS)"

MEDIATOMB_DEPENDENCIES = libcurl libupnp sqlite file spidermonkey

#MEDIATOMB_CONF_ENV += CFLAGS="-lnspr4 -lplc4 -lplds4"
#MEDIATOMB_CONF_ENV += CXXFLAGS="-lnspr4 -lplc4 -lplds4"

MEDIATOMB_CONF_OPTS += \
	--disable-curl \
	--disable-youtube \
	--disable-libmagic \
	--disable-zlip \
	--enable-libjs \
	--disable-libmp4v2 \
	--disable-mysql \
	--disable-taglib \
	--disable-ffmpeg \
	--disable-ffmpegtumbnailer \
	--disable-libextractor \
	--disable-id3lib \
	--disable-FLAC \
	--disable-libexif \
	--disable-inotify \
	--disable-tombdebug \
	--with-js-libs=$(TARGET_DIR)/usr/lib \
	--with-js-h=$(TARGET_DIR)/usr/include/js

ifeq ($(BR2_PACKAGE_MEDIATOMB_BLUETOOTH),y)
MEDIATOMB_CONF_OPTS += --enable-bluetooth
endif

define MEDIATOMB_PRE_PATCH_FIX
	echo PREPATCH $(@D)
	pwd
	sed -i -e 's:SEARCH_DIR="/usr/local":SEARCH_DIR="$(TARGET_DIR)/usr":g' $(@D)/configure.ac
	sed -i -e 's:mozjs, JS_NewObject:mozjs185, JS_NewObject:g' $(@D)/configure.ac
	sed -i -e 's:-lmozjs:-lnspr4 -lplc4 -lplds4 -lmozjs185:g' $(@D)/configure.ac
	sed -i -e 's:MOZLIB=mozjs:MOZLIB=mozjs185:g' $(@D)/configure.ac
endef

MEDIATOMB_POST_PATCH_HOOKS += MEDIATOMB_PRE_PATCH_FIX
$(eval $(autotools-package))
