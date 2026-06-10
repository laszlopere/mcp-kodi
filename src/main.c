/* mcp-kodi — an MCP server for controlling Kodi over JSON-RPC.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Entry point. Loads configuration, builds the Kodi client, tool table, and MCP
 * dispatcher, then runs the stdio transport until stdin EOF.
 */

#include "config.h"

#include <glib.h>

#include "mk-config.h"
#include "mk-kodi.h"
#include "mk-mcp.h"
#include "mk-stdio.h"
#include "mk-tools.h"

/**
 * dispatch_trampoline:
 * @message: one parsed JSON-RPC message; borrowed.
 * @user_data: the MkMcp dispatcher.
 *
 * Adapts mk_mcp_dispatch() to the MkStdioDispatch callback signature, so the
 * transport can route each message into the MCP method dispatch.
 *
 * @return the response envelope to send, or NULL for notifications.
 */
static JsonNode *
dispatch_trampoline (JsonNode *message, gpointer user_data)
{
  return mk_mcp_dispatch (user_data, message);
}

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

  g_autoptr (MkMcp) mcp = mk_mcp_new (tools);
  g_autoptr (MkStdio) stdio = mk_stdio_new (dispatch_trampoline, mcp);

  g_printerr ("%s %s — listening on stdio\n", PACKAGE_NAME, PACKAGE_VERSION);
  mk_stdio_run (stdio);

  return 0;
}
