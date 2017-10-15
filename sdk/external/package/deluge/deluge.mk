################################################################################
#
# deluge
#
################################################################################

DELUGE_VERSION = 1.3.12
DELUGE_SITE = http://download.deluge-torrent.org/source
DELUGE_SOURCE = deluge-${DELUGE_VERSION}.tar.bz2
DELUGE_DEPENDENCIES = python host-python python-setuptools \
	python-pyxdg python-service-identity python-chardet \
	python-attrs \
	python-pyopenssl libtorrent-rasterbar \
	boost
DELUGE_SETUP_TYPE = setuptools
DELUGE_LICENSE = GPLv2
DELUGE_LICENSE_FILES = COPYING

$(eval $(python-package))
