/* mcp-kodi — unit tests for the MCP tool table (mk-tools).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * This is the suite that proves the stubbing: mk-tools is linked against
 * stub-kodi.c (no libsoup, no socket) and stub-history.c (no history.json), so a
 * handler can be driven all the way through player_state() with deterministic
 * input and ZERO file or network activity. The stubs also record what happened
 * (which RPCs were issued, whether the history write path was reached) so the
 * tests can assert the handler routed through them.
 *
 * Covered:
 *   - tools/list renders one well-formed entry per tool;
 *   - an unknown tool name is a protocol-level error;
 *   - the `noop` handler issues the expected Kodi RPCs and shapes a result,
 *     entirely against the stubs.
 */

#define MK_TEST_IMPL
#include "mk-test.h"

#include "mk-config.h"
#include "mk-kodi.h"
#include "mk-tools.h"

/* From stub-kodi.c / stub-history.c. */
extern GPtrArray *stub_kodi_methods;
extern void       stub_kodi_reset (void);
extern int        stub_history_record_calls;
extern void       stub_history_reset (void);

/* Build a tools table over a one-instance config and the stub Kodi client.
 * Out-params hand back the owned pieces so the case can free them. */
static MkTools *
make_tools (MkConfig **out_cfg, MkKodi **out_kodi)
{
  stub_kodi_reset ();
  stub_history_reset ();

  MkConfig *cfg = mk_config_new ();
  mk_config_set_instance (cfg, "default",
                          mk_instance_new ("Test Box", "127.0.0.1:8080",
                                           NULL, "http", FALSE, FALSE));
  mk_config_set_default (cfg, "default");

  MkKodi *kodi = mk_kodi_new (cfg);
  MkTools *tools = mk_tools_new (cfg, kodi);

  *out_cfg = cfg;
  *out_kodi = kodi;
  return tools;
}

/* True if the stub recorded a call to METHOD. */
static gboolean
called (const char *method)
{
  if (stub_kodi_methods == NULL)
    return FALSE;
  for (guint i = 0; i < stub_kodi_methods->len; i++)
    if (strcmp (g_ptr_array_index (stub_kodi_methods, i), method) == 0)
      return TRUE;
  return FALSE;
}

static void
case_tools_list_shape (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  guint count = mk_tools_count (tools);
  MK_CHECK (count > 0);

  g_autoptr (JsonNode) list = mk_tools_list (tools);
  MK_CHECK (JSON_NODE_HOLDS_ARRAY (list));

  JsonArray *arr = json_node_get_array (list);
  /* one entry per tool */
  MK_CHECK_INT_EQ (json_array_get_length (arr), count);

  /* every entry is { name:string, description:string, inputSchema:object } */
  gboolean all_well_formed = TRUE;
  gboolean saw_noop = FALSE;
  for (guint i = 0; i < json_array_get_length (arr); i++)
    {
      JsonObject *e = json_array_get_object_element (arr, i);
      if (e == NULL
          || !json_object_has_member (e, "name")
          || !json_object_has_member (e, "description")
          || !JSON_NODE_HOLDS_OBJECT (
               json_object_get_member (e, "inputSchema")))
        {
          all_well_formed = FALSE;
          continue;
        }
      const char *name = json_object_get_string_member (e, "name");
      if (name != NULL && strcmp (name, "noop") == 0)
        saw_noop = TRUE;
    }
  MK_CHECK (all_well_formed);
  MK_CHECK (saw_noop);

  /* listing the table makes no Kodi call */
  MK_CHECK_INT_EQ (stub_kodi_methods->len, 0);

  mk_tools_free (tools);
  mk_kodi_free (kodi);
  mk_config_free (cfg);
}

static void
case_unknown_tool_is_error (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  GError *error = NULL;
  JsonNode *res = mk_tools_call (tools, "no-such-tool", NULL, &error);

  /* unknown tool → NULL return with the UNKNOWN_TOOL domain error */
  MK_CHECK (res == NULL);
  MK_CHECK (error != NULL);
  if (error != NULL)
    {
      MK_CHECK (g_error_matches (error, MK_TOOLS_ERROR,
                                 MK_TOOLS_ERROR_UNKNOWN_TOOL));
      g_clear_error (&error);
    }

  mk_tools_free (tools);
  mk_kodi_free (kodi);
  mk_config_free (cfg);
}

static void
case_noop_routes_through_stub (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  GError *error = NULL;
  g_autoptr (JsonNode) res = mk_tools_call (tools, "noop", NULL, &error);

  MK_CHECK (error == NULL);
  MK_CHECK (res != NULL);

  if (res != NULL)
    {
      /* result envelope: { content: [ { type, text } ], isError: false } */
      JsonObject *o = json_node_get_object (res);
      MK_CHECK (json_object_has_member (o, "content"));
      MK_CHECK (!json_object_get_boolean_member (o, "isError"));

      /* the snapshot is JSON text inside content[0].text */
      JsonArray *content = json_object_get_array_member (o, "content");
      JsonObject *c0 = json_array_get_object_element (content, 0);
      const char *text = json_object_get_string_member (c0, "text");
      g_autoptr (JsonNode) snap = json_from_string (text, NULL);
      MK_CHECK (snap != NULL && JSON_NODE_HOLDS_OBJECT (snap));
      if (snap != NULL && JSON_NODE_HOLDS_OBJECT (snap))
        {
          JsonObject *s = json_node_get_object (snap);
          /* stub reports a playing audio song (see stub-kodi.c) */
          MK_CHECK_STR_EQ (json_object_get_string_member (s, "state"),
                           "playing");
          MK_CHECK_STR_EQ (json_object_get_string_member (s, "type"), "audio");
        }
    }

  /* noop snapshots the player: GetActivePlayers → GetProperties → GetItem,
   * all served by the stub — no real traffic. */
  MK_CHECK (called ("Player.GetActivePlayers"));
  MK_CHECK (called ("Player.GetProperties"));
  MK_CHECK (called ("Player.GetItem"));

  /* and the snapshot is fed to the history write path (stubbed, no disk) */
  MK_CHECK_INT_EQ (stub_history_record_calls, 1);

  g_clear_error (&error);
  mk_tools_free (tools);
  mk_kodi_free (kodi);
  mk_config_free (cfg);
}

int
main (int argc, char **argv)
{
  static const MkTestCase cases[] = {
    { "tools-list-shape",         case_tools_list_shape },
    { "unknown-tool-is-error",    case_unknown_tool_is_error },
    { "noop-routes-through-stub", case_noop_routes_through_stub },
  };
  return mk_test_run (argc, argv, cases, G_N_ELEMENTS (cases));
}
