################################################################################
#
# bluealsa
#
################################################################################

BLUEALSA_VERSION = master

# this file is run from /sdk/build/x86_64/qemu/output/build/<tempdir>/
BLUEALSA_VERSION = 652bff3affd5f745f2f9f153f2ac3fe3c3cd10cb
BLUEALSA_SITE = git://github.com/Arkq/bluez-alsa.git
BLUEALSA_SITE_METHOD = git
BLUEALSA_REDISTRIBUTE = YES
BLUEALSA_LICENSE = OTHER
BLUEALSA_LICENSE_FILES = COPYING
BLUEALSA_INSTALL_STAGING = NO
BLUEALSA_AUTORECONF = YES
BLUEALSA_DEPENDENCIES = bluez5_utils ortp sbc libbsd

BLUEALSA_CONF_OPTS = --disable-payloadcheck

define BLUEALSA_MAKEM4
mkdir -pv $(BUILD_DIR)/bluealsa-652bff3affd5f745f2f9f153f2ac3fe3c3cd10cb/m4
endef

BLUEALSA_POST_EXTRACT_HOOKS += BLUEALSA_MAKEM4

$(eval $(autotools-package))
