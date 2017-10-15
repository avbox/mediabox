################################################################################
#
# bluealsa
#
################################################################################

BLUEALSA_VERSION = master

# this file is run from /sdk/build/x86_64/qemu/output/build/<tempdir>/
BLUEALSA_SITE = git://github.com/khvzak/bluez-tools.git
BLUEALSA_SITE_METHOD = git
BLUEALSA_REDISTRIBUTE = NO
BLUEALSA_LICENSE = OTHER
BLUEALSA_LICENSE_FILES = COPYING
BLUEALSA_INSTALL_STAGING = NO
BLUEALSA_AUTORECONF = YES
BLUEALSA_DEPENDENCIES = readline

$(eval $(autotools-package))
