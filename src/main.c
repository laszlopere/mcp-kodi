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

/* Live-monitoring poll cadence, in seconds (TODO 11.9.5). ~2 minutes: fine
 * enough that playback started outside the server (TV remote) lands in the
 * history while it is still playing, coarse enough to be invisible on the
 * LAN. MCP_KODI_POLL_SECONDS overrides it (0 disables polling) — for tests,
 * which cannot wait two minutes, but also an honest user knob. */
#define MK_POLL_INTERVAL_SECONDS 120

/**
 * poll_trampoline:
 * @user_data: the MkTools table.
 *
 * Adapts mk_tools_poll_history() to the GSourceFunc signature for the
 * recurring live-monitoring timer.
 *
 * @return G_SOURCE_CONTINUE, always — the poller runs for the server's life.
 */
static gboolean
poll_trampoline (gpointer user_data)
{
  mk_tools_poll_history (user_data);
  return G_SOURCE_CONTINUE;
}

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

  const char *default_name = mk_config_get_default (cfg);
  g_printerr ("%s %s — config loaded: %u instance(s), default=%s\n",
              PACKAGE_NAME, PACKAGE_VERSION,
              mk_config_instance_count (cfg),
              default_name ? default_name : "(none yet)");

  g_autoptr (MkKodi) kodi = mk_kodi_new (cfg);
  g_autoptr (MkTools) tools = mk_tools_new (cfg, kodi);

  g_printerr ("%s %s — %u tool(s) registered\n", PACKAGE_NAME, PACKAGE_VERSION,
              mk_tools_count (tools));

  g_autoptr (MkMcp) mcp = mk_mcp_new (tools);
  g_autoptr (MkStdio) stdio = mk_stdio_new (dispatch_trampoline, mcp);

  /* Live monitoring (11.9.5): poll the boxes on a timer so the history log
   * also sees playback this server did not cause. Websocket notifications may
   * or may not be available per box, so we depend only on the timer. The
   * source lives on the default main context, which mk_stdio_run() drives. */
  guint poll_seconds = MK_POLL_INTERVAL_SECONDS;
  const char *poll_env = g_getenv ("MCP_KODI_POLL_SECONDS");
  if (poll_env != NULL)
    poll_seconds = (guint) g_ascii_strtoull (poll_env, NULL, 10);
  if (poll_seconds > 0)
    {
      g_timeout_add_seconds (poll_seconds, poll_trampoline, tools);
      g_printerr ("%s %s — history poll every %us\n", PACKAGE_NAME,
                  PACKAGE_VERSION, poll_seconds);
    }

  g_printerr ("%s %s — listening on stdio\n", PACKAGE_NAME, PACKAGE_VERSION);
  mk_stdio_run (stdio);

  return 0;
}
