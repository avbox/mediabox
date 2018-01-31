This directory contains everything you need build a kernel and and a MediaBox
root filesystem. Everything else (compiler and host toolchain) is provided by
your distro's essentials package or whatever it's called.


external/packages/ - Buildroot recipes for building MediaBox and it's deps

config/<arch>/<board>/ - Configuration files for Buildroot, Linux, and
  any other files needed to generate the root filesystem for a given
  architecture and board.

temp/ - Temporary files used by the build system

buildroot - Shell script that downloads buildroot, extracts and configures
  it using the config files under config/ for the given architecture and
  board, and starts the build process automatically. If everything goes well
  the script should generate a kernel and root filesystem (and perhaps a
  bootable image depending on arch) under build/<arch>/<board>/output/images.
  Please run ./builroot --help from this directory and refer to the Buildroot
  documentation for more details.


