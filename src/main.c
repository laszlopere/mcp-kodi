/* mcp-kodi — an MCP server for controlling Kodi over JSON-RPC.
 *
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Entry point. For now this is a scaffold that proves the toolchain and
 * dependencies link; the MCP stdio loop, Kodi client, and tool table are
 * built out in subsequent steps (see ../TODO.md).
 */

#include "config.h"

#include <glib.h>

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_printerr ("%s %s — scaffold (not yet functional)\n",
              PACKAGE_NAME, PACKAGE_VERSION);

  return 0;
}
