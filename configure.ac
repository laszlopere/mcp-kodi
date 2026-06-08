#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([mcp-kodi], [0.1.0], [laszlopere@gmail.com])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([1.16 foreign subdir-objects -Wall -Werror])
AM_SILENT_RULES([yes])

# Checks for programs.
AC_PROG_CC
AM_PROG_AR
LT_INIT

PKG_PROG_PKG_CONFIG

# Checks for libraries.
PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.80])
PKG_CHECK_MODULES([GOBJECT], [gobject-2.0 >= 2.80])
PKG_CHECK_MODULES([GIO], [gio-2.0 >= 2.80])
PKG_CHECK_MODULES([GIO_UNIX], [gio-unix-2.0 >= 2.80])
PKG_CHECK_MODULES([JSON_GLIB], [json-glib-1.0 >= 1.6])
PKG_CHECK_MODULES([SOUP], [libsoup-3.0 >= 3.0])

# Aggregate flags for the binary.
MCP_KODI_CFLAGS="$GLIB_CFLAGS $GOBJECT_CFLAGS $GIO_CFLAGS $GIO_UNIX_CFLAGS $JSON_GLIB_CFLAGS $SOUP_CFLAGS"
MCP_KODI_LIBS="$GLIB_LIBS $GOBJECT_LIBS $GIO_LIBS $GIO_UNIX_LIBS $JSON_GLIB_LIBS $SOUP_LIBS"
AC_SUBST([MCP_KODI_CFLAGS])
AC_SUBST([MCP_KODI_LIBS])

AC_CONFIG_FILES([
  Makefile
  src/Makefile
])
AC_OUTPUT

echo "
  $PACKAGE_NAME $PACKAGE_VERSION

  prefix:        $prefix
  compiler:      $CC
  CFLAGS:        $MCP_KODI_CFLAGS
"
