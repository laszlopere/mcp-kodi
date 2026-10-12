bin_PROGRAMS = mcp-kodi

mcp_kodi_SOURCES = \
	main.c \
	mk-config.c  mk-config.h \
	mk-kodi.c    mk-kodi.h \
	mk-history.c mk-history.h \
	mk-tools.c   mk-tools.h \
	mk-stdio.c   mk-stdio.h \
	mk-mcp.c     mk-mcp.h \
	mk-build-id.c mk-build-id.h

# Module sources are added here as they land:
#	mk-state.c  mk-state.h

# build-id.h holds MK_BUILD_ID: the version plus `git describe` of the source
# tree. It is regenerated on every make (FORCE) but only rewritten when the ID
# changes, so an unchanged tree does not relink. Without a .git (a release
# tarball) it is the plain version; the -e test keeps a tarball unpacked inside
# some other repository from picking up that repository's describe.
nodist_mcp_kodi_SOURCES = build-id.h
BUILT_SOURCES = build-id.h
CLEANFILES = build-id.h

build-id.h: FORCE
	$(AM_V_GEN)id='$(PACKAGE_VERSION)'; \
	if test -e '$(top_srcdir)/.git' \
	   && d=`git -C '$(top_srcdir)' describe --always --dirty 2>/dev/null`; then \
	  id="$$id+$$d"; \
	fi; \
	printf '#define MK_BUILD_ID "%s"\n' "$$id" > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

FORCE:
.PHONY: FORCE

mcp_kodi_CFLAGS = $(MCP_KODI_CFLAGS) -Wall -Wextra
mcp_kodi_LDADD  = $(MCP_KODI_LIBS)
