AC_PREREQ([2.69])
AC_INIT([mediabox], [0.01], [https://github.com/mediabox/])

AC_CONFIG_HEADERS(src/config.h)

AM_INIT_AUTOMAKE([1.9 -Wall std-options gnu])


AC_PROG_CC
AC_PATH_PROG([RM], [rm])
AC_HEADER_STDC
AC_CHECK_HEADERS(unistd.h string.h pwd.h sys/stat.h sys/types.h)

AC_COPYRIGHT([Copyright (C) 2016 Fernando Rodriguez])
AC_REVISION([$Revision: 266 $])

AC_ARG_ENABLE([debug], [extra debugging code])
AM_CONDITIONAL(ENABLE_DEBUG, test x"$enable_debug" = xyes)
if test x"$enable_debug" = xyes; then
	AC_DEFINE([DEBUG],1,[Define to 1 to compile debug code])
	CFLAGS="-ggdb -O1 -DDEBUG -DHAVE_CONFIG_H"
else
	AC_DEFINE([NDEBUG],1,[Define to 1  to disable debug code])
	CFLAGS="-O2 -DNDEBUG -DHAVE_CONFIG_H"
fi


AC_ARG_ENABLE([ionice], [realtime IO priority for main process])
if test x"$enable_ionice" = xyes; then
	AC_DEFINE([ENABLE_IONICE], 1, [Define to 1 to enable ionice])
fi


#AC_SUBST(DAEMON_ARGS)

#
# POSIX Threads
# -------------
#
AX_PTHREAD([],[AC_MSG_ERROR([POSIX threads are required to build this program])])

# We are only building threads programs so we add the pthread flags directly
# to the default compilation variables. This will also allow the tests below
# (fuse, libupnp, ...) to pick these flags.
LIBS="-$PTHREAD_LIBS $LIBS"

LDFLAGS="$LDFLAGS $PTHREAD_CFLAGS"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
CC="$PTHREAD_CC"

PKG_CHECK_MODULES([PANGOCAIRO], [pangocairo], ,
AC_MSG_ERROR('Unable to find pango library. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $PANGOCAIRO_CFLAGS"
LDFLAGS="$LDFLAGS $PANGOCAIRO_LIBS"

PKG_CHECK_MODULES(DIRECTFB, [directfb >= 1.7.6], ,
AC_MSG_ERROR('Unable to find DirectFB. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $DIRECTFB_CFLAGS"
LDFLAGS="$LDFLAGS $DIRECTFB_LIBS"

PKG_CHECK_MODULES(ALSA, [alsa], ,
AC_MSG_ERROR('Unable to find libasound. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $ALSA_CFLAGS"
LDFLAGS="$LDFLAGS $ALSA_LIBS"

PKG_CHECK_MODULES(LIBAVUTIL, [libavutil], ,
AC_MSG_ERROR('Unable to find libavutil. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $LIBAVUTIL_CFLAGS"
LDFLAGS="$LDFLAGS $LIBAVUTIL_LIBS"
PKG_CHECK_MODULES(LIBAVFORMAT, [libavformat], ,
AC_MSG_ERROR('Unable to find libavformat. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $LIBAVFORMAT_CFLAGS"
LDFLAGS="$LDFLAGS $LIBAVFORMAT_LIBS"
PKG_CHECK_MODULES(LIBAVCODEC, [libavcodec], ,
AC_MSG_ERROR('Unable to find libavcodec. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $LIBAVCODEC_CFLAGS"
LDFLAGS="$LDFLAGS $LIBAVCODEC_LIBS"
PKG_CHECK_MODULES(LIBAVFILTER, [libavfilter], ,
AC_MSG_ERROR('Unable to find libavfilter. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $LIBAVFILTER_CFLAGS"
LDFLAGS="$LDFLAGS $LIBAVFILTER_LIBS"

#
# libcurl
#
AX_LIB_CURL([7.45.0],,
	[AC_MSG_ERROR([Your system lacks libcurl >= 7.45.0])])
LDFLAGS="$LDFLAGS -lcurl"
AC_DEFINE([USE_CURL], 1, [Define to 1 to use libcurl])


AC_SUBST(PREFIX)

AC_OUTPUT(Makefile \
	src/Makefile)
