#!/bin/sh


# for some reason (no udev?) directfb wants to use these
# drivers before the fbdev driver. So delete them
DFB_SYSTEMS="$(realpath output/target/usr/lib/directfb-1.7-7/systems)"
if [ -e "${DFB_SYSTEMS}/libdirectfb_devmem.so" ]; then
	rm -v "${DFB_SYSTEMS}/libdirectfb_devmem.so";
fi
if [ -e "${DFB_SYSTEMS}/libdirectfb_dummy.so" ]; then
	rm -v "${DFB_SYSTEMS}/libdirectfb_dummy.so";
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


# directfb files
rm -rvf output/target/usr/bin/dfbplay
rm -rvf output/target/usr/bin/dfbshow
rm -rvf output/target/usr/bin/dfbswitch

# libevdev
rm -rvf output/target/usr/bin/libevdev-*
rm -rvf output/target/usr/bin/mtdev-test

# libinput
rm -rvf output/target/usr/bin/libinput-*
rm -rvf output/target/usr/bin/touchpad-*
rm -rvf output/target/usr/bin/mouse-dpi-tool

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
