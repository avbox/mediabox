################################################################################
#
# raspi-gpio
#
################################################################################

RASPI_GPIO_VERSION = master
RASPI_GPIO_SITE = https://github.com/RPi-Distro/raspi-gpio.git
RASPI_GPIO_SITE_METHOD = git
RASPI_GPIO_LICENSE = OTHER
RASPI_GPIO_LICENSE_FILES = COPYING
RASPI_GPIO_AUTORECONF = NO

$(eval $(autotools-package))
