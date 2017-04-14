################################################################################
#
# mediabox
#
################################################################################

MEDIABOX_VERSION = staging
MEDIABOX_SITE = https://bitbucket.org/frodzdev/mediabox.git
MEDIABOX_SITE_METHOD = git
MEDIABOX_LICENSE = OTHER
MEDIABOX_LICENSE_FILES = COPYING
MEDIABOX_INSTALL_STAGING = NO
MEDIABOX_AUTORECONF = YES
MEDIABOX_DEPENDENCIES = libcurl libupnp directfb openssl mediatomb avmount

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

$(eval $(autotools-package))
