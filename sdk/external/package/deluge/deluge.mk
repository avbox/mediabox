################################################################################
#
# deluge
#
################################################################################

DELUGE_VERSION = 1.3.12
DELUGE_SITE = http://download.deluge-torrent.org/source
DELUGE_SOURCE = deluge-${DELUGE_VERSION}.tar.bz2
DELUGE_DEPENDENCIES = python host-python host-python-cffi python-setuptools \
	python-pyxdg python-service-identity python-chardet \
	python-attrs \
	python-pyopenssl libtorrentrb \
	boost
DELUGE_SETUP_TYPE = setuptools
DELUGE_LICENSE = GPLv2
DELUGE_LICENSE_FILES = COPYING

define DELUGE_PATCH_LIBDIR
	sed -i -e "s|sysconfig.get_config_var(\"LIBDIR\")|\'$(TARGET_DIR)/usr/lib'|g" \
		output/build/deluge-1.3.12/setup.py
	sed -i -e "s|'/opt/local/lib'|'$(TARGET_DIR)/opt/local/lib'|g" \
		output/build/deluge-1.3.12/setup.py
	sed -i -e "s|'/usr/local/lib'|'$(TARGET_DIR)/usr/local/lib'|g" \
		output/build/deluge-1.3.12/setup.py
endef

DELUGE_POST_PATCH_HOOKS += DELUGE_PATCH_LIBDIR

$(eval $(python-package))
