################################################################################
#
# python-pyxdg
#
################################################################################

PYTHON_PYXDG_VERSION = 0.25
PYTHON_PYXDG_SITE = https://people.freedesktop.org/~takluyver
PYTHON_PYXDG_SOURCE = pyxdg-${PYTHON_PYXDG_VERSION}.tar.gz
PYTHON_PYXDG_DEPENDENCIES = python
PYTHON_PYXDG_SETUP_TYPE = distutils
PYTHON_PYXDG_LICENSE = GPLv2
PYTHON_PYXDG_LICENSE_FILES = COPYING

$(eval $(python-package))
