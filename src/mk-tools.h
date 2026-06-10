/* mcp-kodi — tool table: schemas + handlers.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * The MCP tool surface: a static table of tools, each with a JSON Schema
 * `inputSchema` and a handler that drives the Kodi client (mk-kodi).
 * `tools/list` renders the table; `tools/call` dispatches by name and shapes
 * the result — tool-level failures become `isError` results, not protocol
 * errors. Per-tool handlers land incrementally; until a handler is wired, its
 * tool returns a clean "not implemented" result.
 */

#ifndef MK_TOOLS_H
#define MK_TOOLS_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "mk-config.h"
#include "mk-kodi.h"

G_BEGIN_DECLS

#define MK_TOOLS_ERROR (mk_tools_error_quark ())
GQuark mk_tools_error_quark (void);

typedef enum
{
  MK_TOOLS_ERROR_UNKNOWN_TOOL, /* no tool of that name in the table */
  MK_TOOLS_ERROR_INVALID_ARGS, /* arguments missing/ill-typed for the tool */
} MkToolsError;

typedef struct _MkTools MkTools;

/* Borrows @config and @kodi (does not take ownership); both must outlive the
 * table. */
MkTools *mk_tools_new  (MkConfig *config, MkKodi *kodi);
void     mk_tools_free (MkTools *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MkTools, mk_tools_free)

/* Number of tools in the table. */
guint mk_tools_count (MkTools *self);

/* Build the `tools/list` array: one `{ name, description, inputSchema }` object
 * per tool. Returns a newly allocated JSON array node (free with
 * json_node_unref()); the caller wraps it as `{ "tools": <array> }`. */
JsonNode *mk_tools_list (MkTools *self);

/* Dispatch a `tools/call`. @arguments is the call's "arguments" object
 * (may be NULL). On a known tool — whether it succeeds or fails — returns the
 * result object `{ "content": [ { "type": "text", "text": "<json>" } ],
 * "isError": <bool> }`: a handler failure is shaped as an `isError` result with
 * the detail as JSON text — `{ "error": "<message>" }`, and for a
 * server↔player communication failure also `"category"` + a setup `"hint"` —
 * never a NULL/error return. Returns NULL with
 * @error set to MK_TOOLS_ERROR_UNKNOWN_TOOL only when @name is not in the table,
 * so the caller can map that to a protocol-level error. Free the result with
 * json_node_unref(). */
JsonNode *mk_tools_call (MkTools     *self,
                         const char  *name,
                         JsonObject  *arguments,
                         GError     **error);

/* One round of live monitoring: snapshot every configured instance's
 * now-playing state, which feeds the playback-history log exactly as a tool
 * call would — so the log also captures playback not initiated through this
 * server (e.g. started from the TV remote). An unreachable box is normal
 * (powered off) and is skipped silently. Driven by a timer in main(); never
 * fails. */
void mk_tools_poll_history (MkTools *self);

G_END_DECLS

#endif /* MK_TOOLS_H */
