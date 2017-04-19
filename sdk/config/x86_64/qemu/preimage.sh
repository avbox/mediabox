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

rm -rvf output/target/usr/bin/dfbplay
rm -rvf output/target/usr/bin/dfbshow

# fix libmozjs symlinks. TODO: This needs to be done
# by spidermonkey's makefile
ln -sf libmozjs185.so.1.0.0 output/target/usr/lib/libmozjs185.so
ln -sf libmozjs185.so.1.0.0 output/target/usr/lib/libmozjs185.so.1.0


rm -rvf output/target/usr/bin/deluge
rm -rvf output/target/usr/bin/deluge-gtk
rm -rvf output/target/usr/bin/deluge-web
ln -s deluge-console output/target/usr/bin/deluge

# create log directory (by default it's a symlink to /tmp!)
rm -rvf output/target/var/log
mkdir -p output/target/var/log
