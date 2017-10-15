################################################################################
#
# mediabox
#
################################################################################

MEDIABOX_VERSION = staging

# this file is run from /sdk/build/x86_64/qemu/output/build/<tempdir>/
MEDIABOX_SITE = ../../../../../../../.git
MEDIABOX_SITE_METHOD = git
MEDIABOX_REDISTRIBUTE = NO
MEDIABOX_LICENSE = OTHER
MEDIABOX_LICENSE_FILES = COPYING
MEDIABOX_INSTALL_STAGING = NO
MEDIABOX_AUTORECONF = YES
MEDIABOX_DEPENDENCIES = libcurl libupnp directfb openssl mediatomb avmount bluealsa

MEDIABOX_CONF_OPTS = --without-systemd

ifeq ($(BR2_PACKAGE_MEDIABOX_DEBUG),y)
MEDIABOX_CONF_OPTS += --enable-debug
endif

ifeq ($(BR2_PACKAGE_MEDIABOX_BLUETOOTH),y)
MEDIABOX_CONF_OPTS += --enable-bluetooth
endif

ifeq ($(BR2_PACKAGE_MEDIABOX_LIBDRM),y)
MEDIABOX_CONF_OPTS += --enable-libdrm
endif

# remove from package cache
define MEDIABOX_UNCACHE
rm -f $(DL_DIR)/mediabox-staging.tar.gz
endef

MEDIABOX_PRE_DOWNLOAD_HOOKS += MEDIABOX_UNCACHE

$(eval $(autotools-package))
