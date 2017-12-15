################################################################################
#
# spidermonkey
#
################################################################################

SPIDERMONKEY_VERSION = 1.8.5
SPIDERMONKEY_SITE = https://ftp.mozilla.org/pub/mozilla.org/js
SPIDERMONKEY_SOURCE = js185-1.0.0.tar.gz
SPIDERMONKEY_LICENSE = MOZ
SPIDERMONKEY_LICENSE_FILES = COPYING
SPIDERMONKEY_SUBDIR = js/src

SPIDERMONKEY_CONF_OPTS += \
	--prefix=$(TARGET_DIR)/usr \
	--enable-threadsafe \
	--disable-nspr-build \
	--with-nspr-cflags="-I $(TARGET_DIR)/usr/include/nspr" \
	--with-nspr-libs="$(TARGET_DIR)/usr/lib/libnspr4.so $(TARGET_DIR)/usr/lib/libplc4.so \
		$(TARGET_DIR)/usr/lib/libplds4.so" \
	--enable-jemalloc \
	--disable-readline \
	--disable-optimize \
	--disable-profile-guided-optimization \
	--enable-debug \
	--disable-tests \
	--enable-polyic \
	--enable-methodjit \
	--enable-methodjit-spew \
	--enable-tracejit \
	--host=$(GNU_HOST_NAME) \
	--target=$(GNU_TARGET_NAME) \
	--build=$(GNU_TARGET_NAME)


SPIDERMONKEY_DEPENDENCIES = pkgconf nspr

define SPIDERMONKEY_BUILD_CMDS
	/bin/sh -c ' \
		mkdir -p $(@D)/js/src/build_OPT.OBJ/dist; \
		cd $(@D)/js/src/build_OPT.OBJ; \
		export CROSS_COMPILE=1; \
		export HOST_CC=/usr/bin/gcc; \
		export HOST_CXX=/usr/bin/g++; \
		export HOST_CPP="/usr/bin/g++ -E"; \
		export HOST_AR=/usr/bin/ar; \
		export HOST_AS=/usr/bin/as; \
		export HOST_RANLIB=/usr/bin/ranlib; \
		export CC=$(HOST_DIR)/usr/bin/$(GNU_TARGET_NAME)-gcc; \
		export CXX=$(HOST_DIR)/usr/bin/$(GNU_TARGET_NAME)-g++; \
		export CPP="$(HOST_DIR)/usr/bin/$(GNU_TARGET_NAME)-g++ -E"; \
		export AR=$(HOST_DIR)/usr/bin/$(GNU_TARGET_NAME)-ar; \
		export AS=$(HOST_DIR)/usr/bin/$(GNU_TARGET_NAME)-as; \
		export LD=$(HOST_DIR)/usr/bin/$(GNU_TARGET_NAME)-ld; \
		export RANLIB=$(HOST_DIR)/usr/bin/$(GNU_TARGET_NAME)-ranlib; \
		export PATH=$(HOST_DIR)/usr/bin:${PATH}; \
		sed -i -e "s/ASSERT(m_allowFlush);//g" $(@D)/js/src/assembler/assembler/AssemblerBufferWithConstantPool.h; \
		sed -i -e "s/ -mfloat-abi=softfp//g" $(@D)/js/src/configure.in; \
		sed -i -e "s/ -mfloat-abi=softfp//g" $(@D)/js/src/configure; \
		$(@D)/js/src/configure $(SPIDERMONKEY_CONF_OPTS); \
		$(MAKE) -C config; \
		$(MAKE) CXXFLAGS="-DENABLE_ASSEMBLER=1"; \
	'
endef

define SPIDERMONKEY_INSTALL_TARGET_CMDS
	/bin/sh -c ' \
		cd $(@D)/js/src/build_OPT.OBJ; \
		$(MAKE1) install; \
	'
endef

#$(eval $(host-generic-package))
$(eval $(generic-package))
