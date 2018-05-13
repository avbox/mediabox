AC_PREREQ([2.69])
AC_INIT([mediabox], [0.01], [https://github.com/avbox/mediabox/])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS(src/config.h)
AC_CONFIG_SRCDIR(src/main.c)
AC_CONFIG_MACRO_DIR(m4)
AC_COPYRIGHT([Copyright (C) 2016-2017 Fernando Rodriguez])
AC_REVISION([$Revision: 266 $])

AM_INIT_AUTOMAKE([1.9 -Wall std-options gnu])


AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AC_PROG_RANLIB
AM_PROG_AR
AC_PATH_PROG([RM], [rm])
AC_PROG_CXX

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

AM_CONDITIONAL(ENABLE_LIBTORRENT, test xyes = xyes)
AC_DEFINE([ENABLE_LIBTORRENT], 1, [Define to 1 to enable libtorrent support])


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



#AC_SUBST(DAEMON_ARGS)

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

PKG_CHECK_MODULES(LIBAVUTIL, [libavutil], ,
	AC_MSG_ERROR('Unable to find libavutil. Please make sure library and header files are installed.'))
PKG_CHECK_MODULES(LIBAVFORMAT, [libavformat], ,
	AC_MSG_ERROR('Unable to find libavformat. Please make sure library and header files are installed.'))
PKG_CHECK_MODULES(LIBAVCODEC, [libavcodec], ,
	AC_MSG_ERROR('Unable to find libavcodec. Please make sure library and header files are installed.'))
PKG_CHECK_MODULES(LIBAVFILTER, [libavfilter], ,
	AC_MSG_ERROR('Unable to find libavfilter. Please make sure library and header files are installed.'))

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
	AC_SEARCH_LIBS([onion_new], [onion], [], [
		AC_MSG_ERROR([required library libonion not found])
	])
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
PKG_CHECK_MODULES(LIBTORRENT, [libtorrent-rasterbar >= 1.1.5], ,
	AC_MSG_ERROR('Unable to find libtorrent. Please make sure library and header files are installed.'))


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

YCM_FLAGS="$(flags2ycm ${CFLAGS} \
	${PTHREAD_CFLAGS} \
	${DIRECTFB_CFLAGS} \
	${ALSA_CFLAGS} \
	${PANGOCAIRO_CFLAGS} \
	${CURL_CFLAGS} \
	${LIBAVUTIL_CFLAGS} \
	${LIBAVFORMAT_CFLAGS} \
	${LIBAVCODEC_CFLAGS} \
	${LIBAVFILTER_CFLAGS} \
	${GLIB_CFLAGS} \
	${GIO_CFLAGS} \
	${LIBINPUT_CFLAGS} \
	${SQLITE3_CFLAGS})"


AC_SUBST(PREFIX)
AC_SUBST(YCM_FLAGS)
AM_SUBST_NOTMAKE(YCM_FLAGS)

AC_CONFIG_FILES([Makefile
                 src/Makefile
		 src/.ycm_extra_conf.py
		 src/mediabox.service])
AC_OUTPUT
