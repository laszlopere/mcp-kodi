/* mcp-kodi — an MCP server for controlling Kodi over JSON-RPC.
 *
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Entry point. Loads configuration; the MCP stdio loop, Kodi client, and tool
 * table are built out in subsequent steps (see ../TODO.md §11).
 */

#include "config.h"

#include <glib.h>

#include "mk-config.h"
#include "mk-kodi.h"
#include "mk-tools.h"

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_autoptr (GError) error = NULL;
  g_autoptr (MkConfig) cfg = mk_config_load (NULL, &error);
  if (cfg == NULL)
    {
      g_printerr ("%s %s: %s\n", PACKAGE_NAME, PACKAGE_VERSION, error->message);
      return 1;
    }

  g_printerr ("%s %s — config loaded: %u instance(s), default=%s\n",
              PACKAGE_NAME, PACKAGE_VERSION,
              mk_config_instance_count (cfg), mk_config_get_default (cfg));

  g_autoptr (MkKodi) kodi = mk_kodi_new (cfg);
  g_autoptr (MkTools) tools = mk_tools_new (cfg, kodi);

  g_printerr ("%s %s — %u tool(s) registered\n", PACKAGE_NAME, PACKAGE_VERSION,
              mk_tools_count (tools));

  /* TODO §11.5: set up GMainLoop + stdio MCP transport and run; mk-mcp routes
   * tools/list and tools/call into mk_tools_list()/mk_tools_call() from here. */
  return 0;
}
