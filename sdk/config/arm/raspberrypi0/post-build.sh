#!/bin/sh

BOARD_DIR="$(dirname $0)"

set -u
set -e

# clear /var
mkdir -p ${TARGET_DIR}/data
mkdir -p ${TARGET_DIR}/boot
mkdir -p ${TARGET_DIR}/media/UPnP
#rm -rvf ${TARGET_DIR}/var/*
rm -rvf ${TARGET_DIR}/etc/wpa_supplicant.conf
rm -rvf ${TARGET_DIR}/etc/init.d/S40network
ln -svf /boot/wpa_supplicant.conf ${TARGET_DIR}/etc/wpa_supplicant.conf

# Add a console on tty1
if [ -e ${TARGET_DIR}/etc/inittab ]; then
    grep -qE '^tty1::' ${TARGET_DIR}/etc/inittab || \
	sed -i '/GENERIC_SERIAL/a\
tty1::respawn:/sbin/getty -L  ttyGS0 0 vt100 # HDMI console\' ${TARGET_DIR}/etc/inittab
fi


# junk
rm -rvf output/target/opt
rm -rvf output/target/usr/bin/compile-et.pl
rm -rvf output/target/usr/bin/gettex*
rm -rvf output/target/usr/bin/gapplication
rm -rvf output/target/usr/bin/pango-view
rm -rvf output/target/usr/bin/ngettext
rm -rvf output/target/usr/bin/hb-*
rm -rvf output/target/usr/bin/gsettings
rm -rvf output/target/usr/bin/gresource
rm -rvf output/target/usr/bin/js-config
rm -rvf output/target/usr/bin/nspr-config
rm -rvf output/target/etc/hostname	# we use /boot/hostname
rm -rvf output/target/etc/init.d/S50dropbear # we use inittab


# fix libmozjs symlinks. TODO: This needs to be done
# by spidermonkey's makefile
ln -sf libmozjs185.so.1.0.0 output/target/usr/lib/libmozjs185.so
ln -sf libmozjs185.so.1.0.0 output/target/usr/lib/libmozjs185.so.1.0


# remove unneeded deluge files
rm -rvf output/target/usr/bin/deluge
rm -rvf output/target/usr/bin/deluge-gtk
rm -rvf output/target/usr/bin/deluge-web
ln -s deluge-console output/target/usr/bin/deluge

# remove fontconfig files
rm -rvf output/target/usr/bin/fc-*

# create log directory (by default it's a symlink to /tmp!)
rm -rvf output/target/var/log
mkdir -p output/target/var/log
