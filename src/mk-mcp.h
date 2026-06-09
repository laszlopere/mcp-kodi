/* mcp-kodi — MCP method dispatch.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Routes parsed JSON-RPC messages to the MCP lifecycle methods (§3.3):
 * `initialize`, `notifications/initialized`, `ping`, `tools/list`, and
 * `tools/call`. `tools/list`/`tools/call` defer to the tool table (mk-tools);
 * everything else is answered here. Notifications get no reply (§3.5).
 * See ../TODO.md §3.
 */

#ifndef MK_MCP_H
#define MK_MCP_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "mk-tools.h"

G_BEGIN_DECLS

typedef struct _MkMcp MkMcp;

/* Borrows @tools (does not take ownership); it must outlive the dispatcher. */
MkMcp *mk_mcp_new  (MkTools *tools);
void   mk_mcp_free (MkMcp *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MkMcp, mk_mcp_free)

/* Dispatch one parsed JSON-RPC message (§3.3). @message is borrowed. For a
 * request, returns the response envelope to send (ownership transferred; free
 * with json_node_unref()). For a notification, or any message that warrants no
 * reply, returns NULL (§3.5). Suitable as an MkStdioDispatch via a trampoline
 * that supplies @self as user_data. */
JsonNode *mk_mcp_dispatch (MkMcp *self, JsonNode *message);

G_END_DECLS

#endif /* MK_MCP_H */
