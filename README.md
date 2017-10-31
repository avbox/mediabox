MediaBox
========

MediaBox is a Linux-based embedded media player. It runs directly in
the framebuffer with few dependencies (currently cairo, pango, ffmpeg,
glib, libcurl, libasound, and libsqlite3). It also uses the bluez stack
if bluetooth is enabled. It can use libdrm or DirectFB as video drivers
(an fbdev and an accelerated mesa driver are also on the pipeline) and
libinput or DirectFB for optional keyboard input.

It is designed to be controlled through TCP or Bluetooth through a mobile
remote control app. An Android implementation is included in the source
tree as a reference.

MediaBox also includes a lightweight toolkit for developing embedded
multimedia applications. It is currently on the src/lib directory and
is compiled as part of the mediabox project but the plan is to split it
into a separate project in the future.

It can run on a minimal Linux system and it can even run as PID 1. The
sdk directory includes a script to generate a root image and kernel for
a minimal system. It currently uses buildroot as the build system. The
sdk directory also includes an emulator script to run the generated
images under QEMU. Currently it includes all the configuration files to
generate an x86_64 image that runs under QEMU but it's easy to generate
images for other arches and platforms with buildroot.
