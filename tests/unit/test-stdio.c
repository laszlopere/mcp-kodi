/* mcp-kodi — unit tests for the JSON-RPC response builders (mk-stdio).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Exercises mk_jsonrpc_result() and mk_jsonrpc_error() — pure JSON shaping with
 * no transport involved. The read loop (mk_stdio_run) is never started, so no
 * stdin/stdout or any other I/O happens.
 */

#define MK_TEST_IMPL
#include "mk-test.h"

#include "mk-stdio.h"

/* Build a JsonNode holding an integer id, for the "echo the id" cases. */
static JsonNode *
int_id (gint64 v)
{
  JsonNode *n = json_node_new (JSON_NODE_VALUE);
  json_node_set_int (n, v);
  return n;
}

static void
case_result_echoes_id_and_payload (void)
{
  g_autoptr (JsonNode) id = int_id (7);
  JsonNode *result = json_node_new (JSON_NODE_VALUE);
  json_node_set_string (result, "pong"); /* ownership passes to the builder */

  g_autoptr (JsonNode) resp = mk_jsonrpc_result (id, result);

  MK_CHECK (JSON_NODE_HOLDS_OBJECT (resp));
  JsonObject *o = json_node_get_object (resp);
  MK_CHECK_STR_EQ (json_object_get_string_member (o, "jsonrpc"), "2.0");
  MK_CHECK_INT_EQ (json_object_get_int_member (o, "id"), 7);
  MK_CHECK_STR_EQ (json_object_get_string_member (o, "result"), "pong");
  /* a success response carries no error member */
  MK_CHECK (!json_object_has_member (o, "error"));
}

static void
case_result_null_id (void)
{
  JsonNode *result = json_node_new (JSON_NODE_VALUE);
  json_node_set_boolean (result, TRUE);

  g_autoptr (JsonNode) resp = mk_jsonrpc_result (NULL, result);

  JsonObject *o = json_node_get_object (resp);
  MK_CHECK (json_object_has_member (o, "id"));
  MK_CHECK (json_object_get_null_member (o, "id"));
  MK_CHECK (json_object_get_boolean_member (o, "result"));
}

static void
case_error_shape (void)
{
  g_autoptr (JsonNode) id = int_id (99);

  g_autoptr (JsonNode) resp =
    mk_jsonrpc_error (id, MK_JSONRPC_METHOD_NOT_FOUND, "no such method");

  JsonObject *o = json_node_get_object (resp);
  MK_CHECK_STR_EQ (json_object_get_string_member (o, "jsonrpc"), "2.0");
  MK_CHECK_INT_EQ (json_object_get_int_member (o, "id"), 99);
  MK_CHECK (!json_object_has_member (o, "result"));

  MK_CHECK (json_object_has_member (o, "error"));
  JsonObject *err = json_object_get_object_member (o, "error");
  MK_CHECK_INT_EQ (json_object_get_int_member (err, "code"),
                   MK_JSONRPC_METHOD_NOT_FOUND);
  MK_CHECK_STR_EQ (json_object_get_string_member (err, "message"),
                   "no such method");
}

static void
case_error_null_message_becomes_empty (void)
{
  g_autoptr (JsonNode) resp =
    mk_jsonrpc_error (NULL, MK_JSONRPC_INTERNAL_ERROR, NULL);

  JsonObject *err =
    json_object_get_object_member (json_node_get_object (resp), "error");
  /* a NULL message is normalised to "" rather than emitted as JSON null */
  MK_CHECK_STR_EQ (json_object_get_string_member (err, "message"), "");
}

int
main (int argc, char **argv)
{
  static const MkTestCase cases[] = {
    { "result-echoes-id-and-payload", case_result_echoes_id_and_payload },
    { "result-null-id",               case_result_null_id },
    { "error-shape",                  case_error_shape },
    { "error-null-message-empty",     case_error_null_message_becomes_empty },
  };
  return mk_test_run (argc, argv, cases, G_N_ELEMENTS (cases));
}
