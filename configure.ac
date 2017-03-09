AC_PREREQ([2.69])
AC_INIT([mediabox], [0.01], [https://github.com/avbox/mediabox/])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS(src/config.h)
AC_CONFIG_SRCDIR(src/main.c)
AC_CONFIG_MACRO_DIR(m4)
AC_COPYRIGHT([Copyright (C) 2016 Fernando Rodriguez])
AC_REVISION([$Revision: 266 $])

AM_INIT_AUTOMAKE([1.9 -Wall std-options gnu])


AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AC_PATH_PROG([RM], [rm])

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
])

AC_HEADER_STDC
AC_CHECK_HEADERS(unistd.h string.h pwd.h sys/stat.h sys/types.h)


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

AC_ARG_ENABLE([bluetooth], [Define to 1 to enable Bluetooth support])
AM_CONDITIONAL([ENABLE_BLUETOOTH], [test x$enable_bluetooth = xyes])
if test x"$enable_bluetooth" = xyes; then
	AC_DEFINE([ENABLE_BLUETOOTH], 1, [Define to 1 to enable Bluetooth support])
fi

AC_ARG_ENABLE([branch-optimization], [Define to 1 to enable branch optimizations])
if test x"$enable_branch_optimization" = xyes; then
	AC_DEFINE([ENABLE_BRANCH_OPTIMIZATION], 1, [Define to 1 to enable branch optimization])
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

PKG_CHECK_MODULES(DIRECTFB, [directfb >= 1.4.9], ,
	AC_MSG_ERROR('Unable to find DirectFB. Please make sure library and header files are installed.'))

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
# libcurl
#
AX_LIB_CURL([7.45.0],,
	[AC_MSG_ERROR([Your system lacks libcurl >= 7.45.0])])
AC_DEFINE([USE_CURL], 1, [Define to 1 to use libcurl])


AC_SUBST(PREFIX)

AC_CONFIG_FILES([Makefile
                 src/Makefile
		 src/mediabox.service])
AC_OUTPUT
