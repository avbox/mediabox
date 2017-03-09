################################################################################
#
# nspr
#
################################################################################

NSPR_VERSION = 4.13.1
NSPR_SITE = https://archive.mozilla.org/pub/nspr/releases/v${NSPR_VERSION}/src
NSPR_SOURCE = nspr-${NSPR_VERSION}.tar.gz
NSPR_SUBDIR = nspr
NSPR_LICENSE = MOZ
NSPR_LICENSE_FILES = COPYING

NSPR_CONF_OPTS = --prefix=/usr \
	--host=x86_64-mediabox-linux-uclibc

NSPR_CONF_OPTS += --enable-64bit

define NSPR_BUILD_CMDS
	/bin/sh -c ' \
		mkdir $(@D)/nspr/build_OPT.OBJ; \
		cd $(@D)/nspr/build_OPT.OBJ; \
		export CROSS_COMPILE=1; \
		export HOST_CC=/usr/bin/gcc; \
		export HOST_CXX=/usr/bin/g++; \
		export HOST_AR=/usr/bin/ar; \
		export HOST_RANLIB=/usr/bin/ranlib; \
		export CC=$(HOST_DIR)/usr/bin/x86_64-mediabox-linux-uclibc-gcc; \
		export CXX=$(HOST_DIR)/usr/bin/x86_64-mediabox-linux-uclibc-g++; \
		export LD=$(HOST_DIR)/usr/bin/x86_64-mediabox-linux-uclibc-ld; \
		export AR=$(HOST_DIR)/usr/bin/x86_64-mediabox-linux-uclibc-ar; \
		export RANLIB=$(HOST_DIR)/usr/bin/x86_64-mediabox-linux-uclibc-ranlib; \
		export PATH=$(HOST_DIR)/usr/bin:${PATH}; \
		$(@D)/nspr/configure $(NSPR_CONF_OPTS); \
		$(MAKE); \
	'
endef

define NSPR_INSTALL_TARGET_CMDS
	/bin/sh -c ' \
		cd $(@D)/nspr/build_OPT.OBJ; \
		$(MAKE) DESTDIR=$(TARGET_DIR) install; \
	'
endef

$(eval $(generic-package))
