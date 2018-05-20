AC_PREREQ([2.69])
AC_INIT([mediabox], [0.6], [https://github.com/avbox/mediabox/])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS(src/config.h)
AC_CONFIG_SRCDIR(src/main.c)
AC_CONFIG_MACRO_DIR(m4)
AC_COPYRIGHT([Copyright (C) 2016-2018 Fernando Rodriguez])
AC_REVISION([$Revision: 266 $])

AM_INIT_AUTOMAKE([1.9 -Wall std-options gnu])

AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AC_PROG_RANLIB
AM_PROG_AR
AC_PATH_PROG([RM], [rm])
AC_PROG_CXX

LT_INIT

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_UID_T
AC_C_INLINE
AC_TYPE_INT64_T
AC_TYPE_INT8_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_CHECK_FUNCS([ \
	bzero \
	clock_gettime \
	dup2 \
	gettimeofday \
	inet_ntoa \
	memset \
	mkdir \
	munmap \
	realpath \
	select \
	socket \
	sqrt \
	strcasecmp \
	strdup \
	strerror \
	strstr \
	backtrace \
])

AC_HEADER_STDC
AC_CHECK_HEADERS(unistd.h string.h pwd.h sys/stat.h sys/types.h execinfo.h)


AM_CONDITIONAL(ENABLE_OPENGL, [test xyes = xno])


AC_ARG_ENABLE([debug], [extra debugging code])
AM_CONDITIONAL(ENABLE_DEBUG, test x"$enable_debug" = xyes)
if test x"$enable_debug" = xyes; then
	AC_DEFINE([DEBUG],1,[Define to 1 to compile debug code])
	CFLAGS="-rdynamic -fPIC -ggdb -O1 -DDEBUG -DHAVE_CONFIG_H -D_GNU_SOURCE"
else
	AC_DEFINE([NDEBUG],1,[Define to 1  to disable debug code])
	CFLAGS="-O2 -DNDEBUG -DHAVE_CONFIG_H"
fi

AC_ARG_WITH([system-libtorrent], [Use the system libtorrent-rasterbar library])
AM_CONDITIONAL(WITH_SYSTEM_LIBTORRENT, test x"$with_system_libtorrent" != xno)

AC_ARG_WITH([system-ffmpeg], [Use the system ffmpeg libraries])
AM_CONDITIONAL(WITH_SYSTEM_FFMPEG, test x"$with_system_ffmpeg" != xno)

AC_ARG_WITH([system-libwebsockets], [Use the system libwebsockets libraries])
AM_CONDITIONAL(WITH_SYSTEM_LIBWEBSOCKETS, test x"$with_system_libwebsockets" != xno)

AC_ARG_ENABLE([ionice], [realtime IO priority for main process])
if test x"$enable_ionice" = xyes; then
	AC_DEFINE([ENABLE_IONICE], 1, [Define to 1 to enable ionice])
fi

AC_ARG_ENABLE([bluetooth], [Define to 1 to enable Bluetooth support])
AM_CONDITIONAL([ENABLE_BLUETOOTH], [test x$enable_bluetooth = xyes])
if test x"$enable_bluetooth" = xyes; then
	AC_DEFINE([ENABLE_BLUETOOTH], 1, [Define to 1 to enable Bluetooth support])
fi

AC_ARG_ENABLE([branch-optimization], [Define to 1 to enable branch optimizations])
if test x"$enable_branch_optimization" = xyes; then
	AC_DEFINE([ENABLE_BRANCH_OPTIMIZATION], 1, [Define to 1 to enable branch optimization])
fi

AC_ARG_ENABLE([libinput], [Define to 1 to enable the libinput driver])
AM_CONDITIONAL([ENABLE_LIBINPUT], [test x$enable_libinput = xyes])
if test x"$enable_libinput" = xyes; then
	AC_DEFINE([ENABLE_LIBINPUT], 1, [Define to 1 to enable libinput support])
fi


AC_ARG_ENABLE([directfb], [Define to 1 to enable the DirectFB driver])
AM_CONDITIONAL([ENABLE_DIRECTFB], [test x$enable_directfb = xyes])
if test x"$enable_directfb" = xyes; then
	AC_DEFINE([ENABLE_DIRECTFB], 1, [Define to 1 to enable the DirectFB support])
fi


AC_ARG_ENABLE([libdrm], [Define to 1 to enable the libdrm driver])
AM_CONDITIONAL([ENABLE_LIBDRM], [test x$enable_libdrm = xyes])
if test x"$enable_libdrm" = xyes; then
	AC_DEFINE([ENABLE_LIBDRM], 1, [Define to 1 to enable libdrm support])
fi


AC_ARG_ENABLE([vc4], [Define to 1 to enable the VC4 driver])
AM_CONDITIONAL([ENABLE_VC4], [test x$enable_vc4 = xyes])
if test x"$enable_vc4" = xyes; then
	AC_DEFINE([ENABLE_VC4], 1, [Define to 1 to enable VC4 support])
	AC_DEFINE([ENABLE_OPENGL], 1, [Define to 1 to enable OpenGL support])
	AC_DEFINE([ENABLE_GLES2], 1, [Define to 1 to use GLES2 instead of OpenGL])
	AC_DEFINE([ENABLE_MMAL], 1, [Define to 1 to enable hardware decoding on the VC4])
	AM_CONDITIONAL(ENABLE_OPENGL, [test xyes = xyes])
fi


AC_ARG_ENABLE([gles2], [Define to 1 to enable the X11 driver])
AC_ARG_ENABLE([x11], [Define to 1 to enable the X11 driver])
AM_CONDITIONAL([ENABLE_X11], [test x"$enable_x11" = xyes])
if test x"$enable_x11" = xyes; then
	AC_DEFINE([ENABLE_X11], 1, [Define to 1 to enable X11 support])
	AC_DEFINE([ENABLE_OPENGL], 1, [Define to 1 to enable OpenGL support])
	AM_CONDITIONAL(ENABLE_OPENGL, [test xyes = xyes])
fi

AC_ARG_ENABLE([realtime], [Define to 1 to enable realtime scheduling support])
if test x"$enable_realtime" = xyes; then
	AC_DEFINE([ENABLE_REALTIME], 1, [Define to 1 to enable realtime scheduling support])

	# check that we have MCL_ONFAULT on sys/mman.h
	AC_MSG_CHECKING([if we have MCL_ONFAULT on sys/mman.h])
	AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
		#include "sys/mman.h"
		#ifndef MCL_ONFAULT
		# error macro not defined
		#endif
	]])], [
		AC_DEFINE([HAVE_MCL_ONFAULT], 1, [Define to 1 if we have MCL_ONFAULT on mman.h])
		AC_MSG_RESULT([yes])
	], [
		AC_MSG_RESULT([no])
	])
fi


AC_ARG_ENABLE([dvd], [DVD Disc support])
AM_CONDITIONAL(ENABLE_DVD, test x"$enable_dvd" = xyes)
if test x"$enable_dvd" = xyes; then
	AC_DEFINE([ENABLE_DVD],1,[Define to 1 to enable DVD support])
fi

AC_ARG_ENABLE([dvdcss], [DVD decryption support])
AM_CONDITIONAL(ENABLE_DVDCSS, test x"$enable_dvdcss" = xyes)
if test x"$enable_dvdcss" = xyes; then
	AC_DEFINE([ENABLE_DVDCSS],1,[Define to 1 to enable DVD support])
fi

AC_ARG_ENABLE([webremote], [Define to 1 to enable the web remote])
AM_CONDITIONAL([ENABLE_WEBREMOTE], [test x"$enable_webremote" = xyes])
if test x"$enable_webremote" = xyes; then
	AC_DEFINE([ENABLE_WEBREMOTE], 1, [Define to 1 to enable the web remote])
	AM_CONDITIONAL(ENABLE_WEBREMOTE, [test xyes = xyes])
fi

if test x"$enable_dvdcss" = xyes; then
	if test x"$enable_dvd" != xyes; then
		AC_MSG_ERROR([Cannot enable dvdcss without dvd!])
	fi
fi


#
# POSIX Threads
# -------------
#
AX_PTHREAD([],AC_MSG_ERROR([POSIX threads are required to build this program]))

# We are only building threads programs so we add the pthread flags directly
# to the default compilation variables. This will also allow the tests below
# (fuse, libupnp, ...) to pick these flags.
LIBS="$PTHREAD_LIBS $LIBS"
CC="$PTHREAD_CC"

PKG_CHECK_MODULES([PANGOCAIRO], [pangocairo], ,
	AC_MSG_ERROR('Unable to find pango library. Please make sure library and header files are installed.'))

if test x"$enable_directfb" = xyes; then
	PKG_CHECK_MODULES(DIRECTFB, [directfb >= 1.4.9], ,
		AC_MSG_ERROR('Unable to find DirectFB. Please make sure library and header files are installed.'))
fi

if test x"$enable_libinput" = xyes; then
	PKG_CHECK_MODULES(LIBINPUT, [libinput], ,
		AC_MSG_ERROR('Unable to find libinput. Please make sure library and header files are installed.'))
fi


PKG_CHECK_MODULES(ALSA, [alsa], ,
	AC_MSG_ERROR('Unable to find libasound. Please make sure library and header files are installed.'))
AC_MSG_CHECKING([if we have SND_PCM_TSTAMP_TYPE_MONOTONIC on asoundlib.h])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
	#include "alsa/asoundlib.h"
	const int x = SND_PCM_TSTAMP_TYPE_MONOTONIC;
]])], [
	AC_DEFINE([HAVE_SND_PCM_TSTAMP_TYPE_MONOTONIC], 1,
		[Define to 1 if we have SND_PCM_TSTAMP_TYPE_MONOTONIC on asoundlib.h])
	AC_MSG_RESULT([yes])
], [
	AC_MSG_RESULT([no])
])


if test x"$with_system_ffmpeg" = xno; then
	CFLAGS="${CFLAGS} -I../third_party/ffmpeg"
else
	PKG_CHECK_MODULES(LIBAVUTIL, [libavutil], ,
		AC_MSG_ERROR('Unable to find libavutil. Please make sure library and header files are installed.'))
	PKG_CHECK_MODULES(LIBAVFORMAT, [libavformat], ,
		AC_MSG_ERROR('Unable to find libavformat. Please make sure library and header files are installed.'))
	PKG_CHECK_MODULES(LIBAVCODEC, [libavcodec], ,
		AC_MSG_ERROR('Unable to find libavcodec. Please make sure library and header files are installed.'))
	PKG_CHECK_MODULES(LIBAVFILTER, [libavfilter], ,
		AC_MSG_ERROR('Unable to find libavfilter. Please make sure library and header files are installed.'))
	PKG_CHECK_MODULES(LIBSWSCALE, [libswscale], ,
		AC_MSG_ERROR('Unable to find libswscale. Please make sure library and header files are installed.'))
fi

PKG_CHECK_MODULES(SQLITE3, [sqlite3], ,
	AC_MSG_ERROR('Unable to find SQLite3. Please make sure library and header files are installed.'))

AC_CHECK_HEADER([magic.h], [], [
	AC_MSG_ERROR([required header magic.h not found])
])
AC_SEARCH_LIBS([magic_open], [magic], [], [
	AC_MSG_ERROR([required library libmagic not found])
])

if test x"$enable_dvd" = xyes; then
	PKG_CHECK_MODULES(LIBUDEV, [libudev], ,
		AC_MSG_ERROR('Unable to find libudev. Please make sure library and header files are installed.'))
	PKG_CHECK_MODULES(LIBDVDNAV, [dvdnav], ,
		AC_MSG_ERROR('Unable to find libdvdnav. Please make sure library and header files are installed.'))

fi

if test x"$enable_dvdcss" = xyes; then
	PKG_CHECK_MODULES(LIBDVDCSS, [libdvdcss], ,
		AC_MSG_ERROR('Unable to find libdvdcss. Please make sure library and header files are installed.'))
fi


if test x"$enable_x11" = xyes; then
	AC_SEARCH_LIBS([XOpenDisplay], [X11], [], [
		AC_MSG_ERROR([required library libX not found])
	])
	if test x"$enable_gles2" = xyes; then
		AC_DEFINE([ENABLE_GLES2], 1, [Define to 1 to use GLES2 instead of OpenGL])
		AC_SEARCH_LIBS([glUseProgram], [GLESv2], [], [
			AC_MSG_ERROR([required library libGLESv2 not found])
		])
	else
		AC_SEARCH_LIBS([glBegin], [GL], [], [
			AC_MSG_ERROR([required library libGL not found])
		])
	fi
fi

if test x"$enable_vc4" == xyes; then
	PKG_CHECK_MODULES(EGL, egl, , 
		AC_MSG_ERROR('Unable to find libEGL'))
	AC_SEARCH_LIBS([glUseProgram], [GLESv2], [], [
		AC_MSG_ERROR([required library libGLESv2 not found])
	])
	LIBS="${LIBS} -lmmal -lbcm_host -lmmal_core -lmmal_components -lmmal_util"
fi

# web remote dependencies
if test x"$enable_webremote" = xyes; then
	if test x"$with_system_libwebsockets" != xno; then
		PKG_CHECK_MODULES(LIBWEBSOCKETS, [libwebsockets >= 3.0.0], ,
			AC_MSG_ERROR('Unable to find libwebsockets-3.0.0 or try --without-system-libwebsockets.'))
	fi
fi

#
# libdrm support
#
if test x"$enable_libdrm" = xyes; then
	use_opengl=yes

	PKG_CHECK_MODULES(LIBDRM, [libdrm], ,
		AC_MSG_ERROR('Unable to find libdrm. Please make sure library and header files are installed.'))

	AC_SEARCH_LIBS([glBegin], [GL], [], [use_opengl=no])

	if test x"$use_opengl" = xyes; then
		PKG_CHECK_MODULES(EGL, egl, , [use_opengl=no])
		PKG_CHECK_MODULES(GBM, gbm, , [use_opengl=no])
	fi

	if test x"$use_opengl" = xyes; then
		AC_DEFINE([ENABLE_OPENGL], 1, [Define to 1 to enable OpenGL support])
		AM_CONDITIONAL(ENABLE_OPENGL, [test xyes = xyes])
	else
		AC_MSG_WARN('Unable to find GL dependencies. Disabling')
	fi
fi


#
# Bluetooth support
#
if test x"$enable_bluetooth" = xyes; then
	PKG_CHECK_MODULES(BLUEZ, [bluez], ,
		AC_MSG_ERROR('Unable to find bluez. Please make sure library and header files are installed.'))

	PKG_CHECK_MODULES(GLIB, [glib-2.0], ,
		AC_MSG_ERROR('Unable to find glib. Please make sure library and header files are installed.'))

	PKG_CHECK_MODULES(GIO, [gio-2.0], ,
		AC_MSG_ERROR('Unable to find gio. Please make sure library and header files are installed.'))
fi


#
# libtorrent
#
if test x"$with_system_libtorrent" = xno; then
	AC_CONFIG_SUBDIRS([third_party/libtorrent-rasterbar])
else
	PKG_CHECK_MODULES(LIBTORRENT, [libtorrent-rasterbar >= 1.1.5], ,
		AC_MSG_ERROR('Unable to find libtorrent. Please make sure library and header files are installed.'))
fi


#
# libcurl
#
PKG_CHECK_MODULES(CURL, [libcurl], ,
	AC_MSG_ERROR('Unable to find libcurl. Please make sure library and header files are installed.'))
AC_DEFINE([USE_CURL], 1, [Define to 1 to use libcurl])

flags2ycm()
{
	FLAGS_ARRAY=""
	while [[ "$#" != 0 ]]; do
		FLAGS_ARRAY="${FLAGS_ARRAY}'${1}',"$'\n'
		shift
	done
	echo "${FLAGS_ARRAY}"
}

# CFLAGS for YouCompleteMe.
# TODO: How to support C and C++ in one config?
YCM_FLAGS="${PTHREAD_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${DIRECTFB_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${ALSA_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${PANGOCAIRO_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${CURL_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${LIBAVUTIL_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${LIBAVFORMAT_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${LIBAVCODEC_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${LIBAVFILTER_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${GLIB_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${GIO_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${LIBINPUT_CFLAGS}"
YCM_FLAGS="${YCM_FLAGS} ${SQLITE3_CFLAGS})"
YCM_FLAGS="$(flags2ycm ${YCM_FLAGS})"
AC_SUBST(PREFIX)
AC_SUBST(YCM_FLAGS)
AM_SUBST_NOTMAKE(YCM_FLAGS)

# generate files
AC_CONFIG_FILES([Makefile
                 src/Makefile
		 src/.ycm_extra_conf.py
		 src/mediabox.service])

AC_OUTPUT

# configure bundled ffmpeg
if test x"$with_system_ffmpeg" = xno; then
	AS_ECHO
	AS_ECHO "Configuring ffmpeg..."
	AS_ECHO

	FF_CONFIG_SCRIPT="cd third_party/ffmpeg && ./configure"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-programs"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-doc"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-manpages"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-htmlpages"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-podpages"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-txtpages"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-libxcb"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-libxcb-shm"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-libxcb-xfixes"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-libxcb-shape"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-sdl2"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-xlib"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-encoders"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-muxers"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-devices"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-hwaccels"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-protocols"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-network"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-postproc"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-avdevice"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-bsfs"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-filters"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --enable-filter=aresample "
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --enable-filter=aformat"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --enable-filter=null"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-vdpau"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-vaapi"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-vda"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-cuda"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-cuvid"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --disable-shared"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --enable-protocol=file"
	FF_CONFIG_SCRIPT="${FF_CONFIG_SCRIPT} --prefix=${prefix}"

	AS_ECHO "${FF_CONFIG_SCRIPT}"
	AS_ECHO
	(
		eval "${FF_CONFIG_SCRIPT}"
	)
fi

# configure bundled libwebsockets
if test x"$with_system_libwebsockets" = xno; then
	AS_ECHO
	AS_ECHO "Configuring libwebsockets..."
	AS_ECHO
	LWS_CONFIG_SCRIPT="cd third_party/libwebsockets && cmake ."
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_IPV6=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_LINK_TESTAPPS_DYNAMIC=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_HTTP2=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_STATIC=ON"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_LIBEV=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_LIBEVENT=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_LIBUV=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_PEER_LIMITS=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_SSL=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITHOUT_CLIENT=ON"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITHOUT_TEST_CLIENT=ON"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_CGI=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_HTTP_PROXY=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_ACCESS_LOG=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_SERVER_STATUS=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_LEJP=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_GENERIC_SESSIONS=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_SQLITE3=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_SMTP=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_SOCKS5=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_ZIP_FOPS=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITH_SHARED=OFF"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITHOUT_TEST_SERVER=ON"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITHOUT_TEST_SERVER_EXTPOLL=ON"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITHOUT_TEST_PING=ON"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITHOUT_TESTAPPS=ON"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_WITHOUT_TESTAPPS=ON"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_HAVE_LIBCAP=0"
	LWS_CONFIG_SCRIPT="${LWS_CONFIG_SCRIPT} -DLWS_HAVE_SYS_CAPABILITY_H=0"

	AS_ECHO "${LWS_CONFIG_SCRIPT}"
	AS_ECHO
	( eval "${LWS_CONFIG_SCRIPT}" )
	AS_ECHO
fi
