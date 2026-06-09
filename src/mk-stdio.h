/* mcp-kodi — newline-delimited JSON-RPC transport over stdin/stdout.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * The MCP stdio transport (§3.1, §3.2): a GMainLoop with an async read on
 * stdin, framing one JSON-RPC 2.0 message per line. Each parsed message is
 * handed to a dispatch callback whose returned response (if any) is serialised
 * back as a single line on stdout. Logging goes to stderr only; stdout stays
 * pure protocol. EOF on stdin quits the loop. See ../TODO.md §3.
 */

#ifndef MK_STDIO_H
#define MK_STDIO_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* JSON-RPC 2.0 error codes (§3.4). */
#define MK_JSONRPC_PARSE_ERROR      (-32700)
#define MK_JSONRPC_INVALID_REQUEST  (-32600)
#define MK_JSONRPC_METHOD_NOT_FOUND (-32601)
#define MK_JSONRPC_INVALID_PARAMS   (-32602)
#define MK_JSONRPC_INTERNAL_ERROR   (-32603)

/* Build a JSON-RPC 2.0 success response `{ "jsonrpc": "2.0", "id": <id>,
 * "result": <result> }`. @id is borrowed (copied in); NULL emits `"id": null`.
 * @result ownership is transferred to the returned node. Free the result with
 * json_node_unref(). */
JsonNode *mk_jsonrpc_result (JsonNode *id, JsonNode *result);

/* Build a JSON-RPC 2.0 error response `{ "jsonrpc": "2.0", "id": <id>,
 * "error": { "code": <code>, "message": <message> } }`. @id is borrowed
 * (copied in); NULL emits `"id": null`. Free with json_node_unref(). */
JsonNode *mk_jsonrpc_error (JsonNode *id, int code, const char *message);

typedef struct _MkStdio MkStdio;

/* Handle one successfully parsed incoming JSON-RPC message. @message is borrowed
 * (valid only for the duration of the call). Return a response node to write
 * (ownership transferred to the transport, which serialises and frames it), or
 * NULL to write nothing — used for notifications, which never get a reply
 * (§3.5). */
typedef JsonNode *(*MkStdioDispatch) (JsonNode *message, gpointer user_data);

/* Create the transport bound to stdin/stdout. @dispatch is invoked for each
 * parsed message with @user_data; both must outlive the transport. */
MkStdio *mk_stdio_new  (MkStdioDispatch dispatch, gpointer user_data);
void     mk_stdio_free (MkStdio *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MkStdio, mk_stdio_free)

/* Run the read loop until EOF (or an unrecoverable read error) on stdin, then
 * return. Blocks the calling thread. */
void mk_stdio_run (MkStdio *self);

G_END_DECLS

#endif /* MK_STDIO_H */
