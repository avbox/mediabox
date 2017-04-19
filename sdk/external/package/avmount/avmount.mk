################################################################################
#
# avmount
#
################################################################################

AVMOUNT_VERSION = staging
AVMOUNT_SITE = https://github.com/avbox/avmount.git
AVMOUNT_SITE_METHOD = git
AVMOUNT_LICENSE = OTHER
AVMOUNT_LICENSE_FILES = COPYING
AVMOUNT_AUTORECONF = YES


ifeq ($(BR2_PACKAGE_AVMOUNT_DEBUG),y)
AVMOUNT_CONF_OPTS = --enable-debug
endif

AVMOUNT_DEPENDENCIES = libcurl libupnp

$(eval $(autotools-package))
