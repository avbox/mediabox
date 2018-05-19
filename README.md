MediaBox [![Build Status](https://travis-ci.org/avbox/mediabox.svg?branch=master)](https://travis-ci.org/avbox/mediabox)
========


About
-----

MediaBox is a Linux-based embedded media player. It can stream media via
HTTP, uPnP Media Servers, and Bittorrent. A YouTube provider is also on the
works. It cannot help you find any media to stream. Except for uPnP Media
Servers on the local network, all media must be provided in the form of links through the
remote control interface. It can also download some types of media to a local
library.

It has a web-based control interface and a TCP/Bluetooth interface for
remote control applications. An Android remote app is available and one is
on the works for Windows 10.

Requirements
------------

MediaBox runs on a minimal Linux system with few dependencies. It includes
drivers for the X11 server (OpenGL only), Linux DRM (supports software rendering
as well as OpenGL), VideoCore IV/Raspberry Pi (fully accelerated, including
hardware decoding), and DirectFB. Currently only ALSA is supported for audio
output.

Building
--------

The build system is autotools. Ebuilds for Gentoo are available [here](http://github.com/avbox/portage).
The sdk directory includes a script to generate a root image and kernel for a
minimal system. It currently uses buildroot as the build system. The sdk directory
also includes an emulator script to run the generated images under QEMU and configuration
files to generate images for x86_64 and Raspberry Pi Zero.

Binary images for x86_64 and Raspberry Pi zero are in the works.


libavbox
--------

libavbox a lightweight toolkit for developing embedded multimedia applications.
It is currently on the src/lib directory and is linked statically with the mediabox
binary but it will be split into a separate library in the near future.
