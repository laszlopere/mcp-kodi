/* mcp-kodi — MCP method dispatch. See mk-mcp.h.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 */

#include "config.h"

#include "mk-mcp.h"
#include "mk-stdio.h"

/* MCP protocol versions we speak, most recent first. On `initialize` we echo
 * the client's requested version when it is one of these, else offer our
 * latest. */
static const char *const mk_mcp_protocol_versions[] = {
  "2025-06-18",
  "2025-03-26",
  "2024-11-05",
};
#define MK_MCP_LATEST_PROTOCOL (mk_mcp_protocol_versions[0])

struct _MkMcp
{
  MkTools *tools; /* borrowed; must outlive the dispatcher */
};

/* ---- Small JSON helpers --------------------------------------------------- */

/**
 * empty_object_node:
 *
 * Builds a fresh empty JSON object node `{}`.
 *
 * @return a newly allocated node; free with json_node_unref().
 */
static JsonNode *
empty_object_node (void)
{
  JsonNode *node = json_node_new (JSON_NODE_OBJECT);
  json_node_take_object (node, json_object_new ());
  return node;
}

/**
 * object_member:
 * @obj: the object to read, or NULL.
 * @name: the member name.
 *
 * Returns the object member @name only when it is itself a JSON object, so
 * callers can read a nested `params`/`arguments` object defensively.
 *
 * @return the borrowed member object, or NULL if absent or not an object.
 */
static JsonObject *
object_member (JsonObject *obj, const char *name)
{
  if (obj == NULL || !json_object_has_member (obj, name))
    return NULL;
  JsonNode *node = json_object_get_member (obj, name);
  if (!JSON_NODE_HOLDS_OBJECT (node))
    return NULL;
  return json_node_get_object (node);
}

/* ---- Lifecycle methods ---------------------------------------------------- */

/**
 * negotiate_protocol:
 * @params: the `initialize` params object, or NULL.
 *
 * Picks the protocol version to report: the client's requested
 * `protocolVersion` when we recognise it, otherwise our latest.
 *
 * @return a borrowed static version string.
 */
static const char *
negotiate_protocol (JsonObject *params)
{
  const char *requested =
    params ? json_object_get_string_member_with_default (params,
                                                         "protocolVersion",
                                                         NULL)
           : NULL;
  if (requested != NULL)
    for (gsize i = 0; i < G_N_ELEMENTS (mk_mcp_protocol_versions); i++)
      if (g_strcmp0 (requested, mk_mcp_protocol_versions[i]) == 0)
        return mk_mcp_protocol_versions[i];
  return MK_MCP_LATEST_PROTOCOL;
}

/**
 * handle_initialize:
 * @id: the request id to echo; borrowed.
 * @params: the request params, or NULL.
 *
 * Answers `initialize`: negotiated `protocolVersion`, `capabilities`
 * (`{ "tools": {} }`), and `serverInfo` (`{ name, version }`).
 *
 * @return the response envelope; free with json_node_unref().
 */
static JsonNode *
handle_initialize (JsonNode *id, JsonObject *params)
{
  const char *version = negotiate_protocol (params);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "protocolVersion");
  json_builder_add_string_value (b, version);

  json_builder_set_member_name (b, "capabilities");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "tools");
  json_builder_begin_object (b);
  json_builder_end_object (b);
  json_builder_end_object (b);

  json_builder_set_member_name (b, "serverInfo");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "name");
  json_builder_add_string_value (b, PACKAGE_NAME);
  json_builder_set_member_name (b, "version");
  json_builder_add_string_value (b, PACKAGE_VERSION);
  json_builder_end_object (b);

  json_builder_end_object (b);
  return mk_jsonrpc_result (id, json_builder_get_root (b));
}

/**
 * handle_tools_list:
 * @self: the dispatcher.
 * @id: the request id to echo; borrowed.
 *
 * Answers `tools/list`: wraps the tool table as `{ "tools": [ ... ] }`.
 *
 * @return the response envelope; free with json_node_unref().
 */
static JsonNode *
handle_tools_list (MkMcp *self, JsonNode *id)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "tools");
  json_builder_add_value (b, mk_tools_list (self->tools)); /* takes ownership */
  json_builder_end_object (b);
  return mk_jsonrpc_result (id, json_builder_get_root (b));
}

/**
 * handle_tools_call:
 * @self: the dispatcher.
 * @id: the request id to echo; borrowed.
 * @params: the request params, or NULL.
 *
 * Answers `tools/call`: reads `name` and the optional `arguments`
 * object, then dispatches into the tool table. A handler/tool failure comes
 * back as a normal result with `isError: true`; only an unknown tool
 * name maps to a protocol-level `-32602` invalid-params error.
 *
 * @return the response envelope; free with json_node_unref().
 */
static JsonNode *
handle_tools_call (MkMcp *self, JsonNode *id, JsonObject *params)
{
  const char *name =
    params ? json_object_get_string_member_with_default (params, "name", NULL)
           : NULL;
  if (name == NULL)
    return mk_jsonrpc_error (id, MK_JSONRPC_INVALID_PARAMS,
                             "tools/call requires a string \"name\"");

  JsonObject *arguments = object_member (params, "arguments");

  GError *error = NULL;
  g_autoptr (JsonNode) result =
    mk_tools_call (self->tools, name, arguments, &error);
  if (result == NULL)
    {
      /* Unknown tool name: the `name` argument is invalid. */
      g_autoptr (JsonNode) resp =
        mk_jsonrpc_error (id, MK_JSONRPC_INVALID_PARAMS,
                          error ? error->message : "unknown tool");
      g_clear_error (&error);
      return g_steal_pointer (&resp);
    }
  return mk_jsonrpc_result (id, g_steal_pointer (&result));
}

/* ---- Dispatch ------------------------------------------------------------- */

/**
 * mk_mcp_dispatch:
 * @self: the dispatcher.
 * @message: one parsed JSON-RPC message; borrowed.
 *
 * Routes @message to its MCP method. A message with no `id` is a
 * notification and never gets a reply; a request gets a result or a
 * JSON-RPC error envelope. Malformed envelopes yield `-32600`; unknown methods
 * yield `-32601`.
 *
 * @return the response envelope, or NULL for notifications / no-reply cases.
 */
JsonNode *
mk_mcp_dispatch (MkMcp *self, JsonNode *message)
{
  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (message != NULL, NULL);

  if (!JSON_NODE_HOLDS_OBJECT (message))
    return mk_jsonrpc_error (NULL, MK_JSONRPC_INVALID_REQUEST,
                             "request must be a JSON object");

  JsonObject *obj = json_node_get_object (message);

  /* Presence of "id" distinguishes a request from a notification. */
  gboolean is_request = json_object_has_member (obj, "id");
  JsonNode *id = is_request ? json_object_get_member (obj, "id") : NULL;

  const char *method =
    json_object_get_string_member_with_default (obj, "method", NULL);
  if (method == NULL)
    {
      if (!is_request)
        return NULL; /* malformed notification — ignore silently */
      return mk_jsonrpc_error (id, MK_JSONRPC_INVALID_REQUEST,
                               "missing or non-string \"method\"");
    }

  JsonObject *params = object_member (obj, "params");

  /* Notifications (no id): act where relevant, never reply. */
  if (!is_request)
    return NULL;

  if (g_strcmp0 (method, "initialize") == 0)
    return handle_initialize (id, params);
  if (g_strcmp0 (method, "ping") == 0)
    return mk_jsonrpc_result (id, empty_object_node ());
  if (g_strcmp0 (method, "tools/list") == 0)
    return handle_tools_list (self, id);
  if (g_strcmp0 (method, "tools/call") == 0)
    return handle_tools_call (self, id, params);

  g_autofree char *detail = g_strdup_printf ("method not found: %s", method);
  return mk_jsonrpc_error (id, MK_JSONRPC_METHOD_NOT_FOUND, detail);
}

/* ---- Public API ----------------------------------------------------------- */

/**
 * mk_mcp_new:
 * @tools: the tool table to serve; borrowed, not owned.
 *
 * Creates the dispatcher. @tools must outlive it.
 *
 * @return a newly allocated MkMcp; free with mk_mcp_free().
 */
MkMcp *
mk_mcp_new (MkTools *tools)
{
  g_return_val_if_fail (tools != NULL, NULL);

  MkMcp *self = g_new0 (MkMcp, 1);
  self->tools = tools;
  return self;
}

/**
 * mk_mcp_free:
 * @self: the dispatcher to free, or NULL.
 *
 * Frees the dispatcher. The borrowed tool table is left untouched. Safe to call
 * with NULL.
 */
void
mk_mcp_free (MkMcp *self)
{
  if (self == NULL)
    return;
  g_free (self);
}
