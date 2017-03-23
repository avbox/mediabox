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
MEDIABOX_INSTALL_STAGING = YES
MEDIABOX_AUTORECONF = YES

MEDIABOX_CONF_OPTS = \
	--enable-debug \
	--without-systemd

MEDIABOX_DEPENDENCIES = libcurl libupnp directfb openssl mediatomb avmount

ifeq ($(BR2_PACKAGE_MEDIABOX_BLUETOOTH),y)
MEDIABOX_CONF_OPTS += --enable-bluetooth
endif

$(eval $(autotools-package))
