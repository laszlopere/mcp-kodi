bin_PROGRAMS = mcp-kodi

mcp_kodi_SOURCES = \
	main.c \
	mk-config.c mk-config.h \
	mk-kodi.c   mk-kodi.h

# Module sources are added here as they land (see ../TODO.md §6):
#	mk-stdio.c  mk-stdio.h \
#	mk-mcp.c    mk-mcp.h \
#	mk-tools.c  mk-tools.h \
#	mk-state.c  mk-state.h

mcp_kodi_CFLAGS = $(MCP_KODI_CFLAGS) -Wall -Wextra
mcp_kodi_LDADD  = $(MCP_KODI_LIBS)
