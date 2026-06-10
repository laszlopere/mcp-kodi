/* mcp-kodi — unit tests for the kodi-driven MCP tool handlers (mk-tools).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Companion to test-tools.c. Where that suite covers tools/list, the unknown
 * tool, and noop, this one exercises the OTHER handlers — search, playfile, the
 * transport Buttons, mute/unmute, the rpc escape hatch and its gate, plus a
 * handler failure path — all driven against the *programmable* stub
 * (stub-kodi-prog.c): each handler's Kodi calls are answered with realistic
 * canned `result`s the case sets up first, so there is ZERO network or file I/O.
 * The history write path is stubbed too (stub-history.c, no disk). The stubs
 * record which RPCs were issued, so the tests assert routing as well as shape.
 *
 * No instances set/remove and nothing that saves the config — those would touch
 * the real user config dir; only kodi-driven and read-only handlers are covered.
 */

#define MK_TEST_IMPL
#include "mk-test.h"

#include "mk-config.h"
#include "mk-kodi.h"
#include "mk-tools.h"

/* From stub-kodi-prog.c / stub-history.c. */
extern GPtrArray *stub_kodi_methods;
extern void       stub_kodi_reset (void);
extern void       stub_kodi_set_response (const char *method, const char *json);
extern void       stub_kodi_set_error (const char *method);
extern int        stub_history_record_calls;
extern void       stub_history_reset (void);

/* Build a tools table over a two-instance config and the programmable stub
 * Kodi client. "default" is the (rpc-disabled) default box; "rpcbox" has
 * allow_rpc set, so the rpc-gate tests have both sides to exercise. Out-params
 * hand back the owned pieces so the case can free them. */
static MkTools *
make_tools (MkConfig **out_cfg, MkKodi **out_kodi)
{
  stub_kodi_reset ();
  stub_history_reset ();

  MkConfig *cfg = mk_config_new ();
  mk_config_set_instance (cfg, "default",
                          mk_instance_new ("Test Box", "127.0.0.1:8080",
                                           NULL, "http", FALSE, FALSE));
  mk_config_set_instance (cfg, "rpcbox",
                          mk_instance_new ("RPC Box", "127.0.0.1:8081",
                                           NULL, "http", FALSE, TRUE));
  mk_config_set_default (cfg, "default");

  MkKodi *kodi = mk_kodi_new (cfg);
  MkTools *tools = mk_tools_new (cfg, kodi);

  *out_cfg = cfg;
  *out_kodi = kodi;
  return tools;
}

static void
free_tools (MkTools *tools, MkConfig *cfg, MkKodi *kodi)
{
  mk_tools_free (tools);
  mk_kodi_free (kodi);
  mk_config_free (cfg);
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

/* Parse a JSON literal into an owned node holding an arguments object. */
static JsonNode *
args_node (const char *json)
{
  JsonNode *n = json_from_string (json, NULL);
  g_assert (n != NULL && JSON_NODE_HOLDS_OBJECT (n));
  return n;
}

/* Pull the parsed JSON payload out of a result envelope's content[0].text.
 * Returns a newly allocated node (caller frees), or NULL. @is_error out is set
 * to the envelope's isError flag. */
static JsonNode *
envelope_payload (JsonNode *res, gboolean *is_error)
{
  if (res == NULL || !JSON_NODE_HOLDS_OBJECT (res))
    return NULL;
  JsonObject *o = json_node_get_object (res);
  if (is_error != NULL)
    *is_error = json_object_get_boolean_member (o, "isError");
  JsonArray *content = json_object_get_array_member (o, "content");
  JsonObject *c0 = json_array_get_object_element (content, 0);
  const char *text = json_object_get_string_member (c0, "text");
  return json_from_string (text, NULL);
}

/* ---- search ---------------------------------------------------------------- */

static void
case_search_movie (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response (
    "VideoLibrary.GetMovies",
    "{ \"movies\": ["
    "    { \"movieid\": 1, \"label\": \"Heat\", \"title\": \"Heat\","
    "      \"file\": \"/m/heat.mkv\", \"year\": 1995 },"
    "    { \"movieid\": 2, \"label\": \"Casino\", \"title\": \"Casino\","
    "      \"file\": \"/m/casino.mkv\", \"year\": 1995 } ],"
    "  \"limits\": { \"start\": 0, \"end\": 2, \"total\": 2 } }");

  g_autoptr (JsonNode) an = args_node ("{ \"type\": \"movie\" }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "search", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);
  MK_CHECK (res != NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  MK_CHECK (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload));
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "type"), "movie");
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "total"), 2);
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "returned"), 2);
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "offset"), 0);
      MK_CHECK_INT_EQ (
        json_array_get_length (json_object_get_array_member (p, "rows")), 2);
    }

  /* movie resolves directly: one GetMovies call, no sublevels. */
  MK_CHECK (called ("VideoLibrary.GetMovies"));
  MK_CHECK (!called ("VideoLibrary.GetTVShows"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

static void
case_search_music (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  /* Artist resolves to id 7; the wide artist-only path then fetches songs. */
  stub_kodi_set_response (
    "AudioLibrary.GetArtists",
    "{ \"artists\": [ { \"artistid\": 7, \"label\": \"Radiohead\" } ],"
    "  \"limits\": { \"total\": 1 } }");
  stub_kodi_set_response (
    "AudioLibrary.GetSongs",
    "{ \"songs\": ["
    "    { \"songid\": 10, \"label\": \"Idioteque\", \"title\": \"Idioteque\","
    "      \"track\": 8, \"file\": \"/s/idioteque.flac\" } ],"
    "  \"limits\": { \"total\": 1 } }");

  g_autoptr (JsonNode) an =
    args_node ("{ \"type\": \"music\", \"artist\": \"Radiohead\" }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "search", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  MK_CHECK (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload));
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "type"), "music");
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "total"), 1);
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "returned"), 1);
      MK_CHECK_INT_EQ (
        json_array_get_length (json_object_get_array_member (p, "rows")), 1);
      /* resolved.artist/artistid echo the matched container. */
      JsonObject *r = json_object_get_object_member (p, "resolved");
      MK_CHECK (r != NULL);
      if (r != NULL)
        {
          MK_CHECK_STR_EQ (json_object_get_string_member (r, "artist"),
                           "Radiohead");
          MK_CHECK_INT_EQ (json_object_get_int_member (r, "artistid"), 7);
        }
    }

  /* music: resolve the artist, then fetch songs — two distinct RPCs. */
  MK_CHECK (called ("AudioLibrary.GetArtists"));
  MK_CHECK (called ("AudioLibrary.GetSongs"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* ---- playfile -------------------------------------------------------------- */

static void
case_playfile (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response ("Player.Open", "true");
  stub_kodi_set_response ("Player.GetActivePlayers",
                          "[ { \"playerid\": 1, \"type\": \"video\" } ]");
  stub_kodi_set_response (
    "Player.GetProperties",
    "{ \"speed\": 1,"
    "  \"time\": { \"hours\": 0, \"minutes\": 0, \"seconds\": 5,"
    "              \"milliseconds\": 0 },"
    "  \"totaltime\": { \"hours\": 1, \"minutes\": 30, \"seconds\": 0,"
    "                   \"milliseconds\": 0 } }");
  stub_kodi_set_response (
    "Player.GetItem",
    "{ \"item\": { \"title\": \"Heat\", \"file\": \"/m/heat.mkv\","
    "  \"label\": \"Heat\", \"type\": \"movie\", \"id\": 1 } }");

  g_autoptr (JsonNode) an = args_node ("{ \"file\": \"/m/heat.mkv\" }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "playfile", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "state"), "playing");
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "type"), "video");
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "media"), "movie");
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "id"), 1);
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "file"), "/m/heat.mkv");
    }

  /* Player.Open then a player_state() snapshot. */
  MK_CHECK (called ("Player.Open"));
  MK_CHECK (called ("Player.GetActivePlayers"));
  MK_CHECK (called ("Player.GetProperties"));
  MK_CHECK (called ("Player.GetItem"));
  /* the snapshot feeds the history write path (stubbed, no disk). */
  MK_CHECK_INT_EQ (stub_history_record_calls, 1);

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* ---- queue ----------------------------------------------------------------- */

/* The happy path: an episode is playing off video playlist 1, and queueing the
 * next one by library id appends it there — Playlist.Add, never Insert — then
 * returns the (unchanged) player-state snapshot. One Player.GetProperties
 * response serves both the handler's playlistid/position read and the
 * snapshot's speed/time read; each ignores the other's fields. */
static void
case_queue_append (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response ("Player.GetActivePlayers",
                          "[ { \"playerid\": 1, \"type\": \"video\" } ]");
  stub_kodi_set_response (
    "Player.GetProperties",
    "{ \"playlistid\": 1, \"position\": 0, \"speed\": 1,"
    "  \"time\": { \"hours\": 0, \"minutes\": 1, \"seconds\": 0,"
    "              \"milliseconds\": 0 },"
    "  \"totaltime\": { \"hours\": 0, \"minutes\": 30, \"seconds\": 2,"
    "                   \"milliseconds\": 0 } }");
  stub_kodi_set_response ("Playlist.Add", "\"OK\"");
  stub_kodi_set_response (
    "Player.GetItem",
    "{ \"item\": { \"title\": \"The End\", \"file\": \"/v/rd-1x01.avi\","
    "  \"label\": \"The End\", \"type\": \"episode\", \"id\": 30584 } }");

  g_autoptr (JsonNode) an =
    args_node ("{ \"type\": \"episode\", \"id\": 30585 }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "queue", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      /* playback unchanged: the snapshot still shows the current episode. */
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "state"), "playing");
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "id"), 30584);
    }

  /* resolve the player, read its playlist, append — no Insert. */
  MK_CHECK (called ("Player.GetActivePlayers"));
  MK_CHECK (called ("Player.GetProperties"));
  MK_CHECK (called ("Playlist.Add"));
  MK_CHECK (!called ("Playlist.Insert"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* next:true inserts at position+1 (Playlist.Insert, never Add), so the item
 * plays right after the current one; a `file` item works in place of an id. */
static void
case_queue_next_inserts (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response ("Player.GetActivePlayers",
                          "[ { \"playerid\": 1, \"type\": \"video\" } ]");
  stub_kodi_set_response ("Player.GetProperties",
                          "{ \"playlistid\": 1, \"position\": 3, \"speed\": 1 }");
  stub_kodi_set_response ("Playlist.Insert", "\"OK\"");
  stub_kodi_set_response (
    "Player.GetItem",
    "{ \"item\": { \"title\": \"Now\", \"file\": \"/v/now.mkv\","
    "  \"type\": \"episode\", \"id\": 7 } }");

  g_autoptr (JsonNode) an =
    args_node ("{ \"file\": \"/v/next.mkv\", \"next\": true }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "queue", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);

  MK_CHECK (called ("Playlist.Insert"));
  MK_CHECK (!called ("Playlist.Add"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* The one "nothing queueable is playing" error covers both no active player
 * (empty GetActivePlayers) and non-playlist playback (playlistid -1, live
 * TV/streams); neither path may reach Playlist.Add. */
static void
case_queue_nothing_playing (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  /* No active player at all. */
  stub_kodi_set_response ("Player.GetActivePlayers", "[]");

  g_autoptr (JsonNode) an = args_node ("{ \"file\": \"/v/next.mkv\" }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "queue", json_node_get_object (an), &error);

  MK_CHECK (error == NULL); /* tool error, not a protocol error */

  gboolean is_error = FALSE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      const char *msg = json_object_get_string_member (
        json_node_get_object (payload), "error");
      MK_CHECK (msg != NULL && strstr (msg, "nothing queueable") != NULL);
    }
  MK_CHECK (!called ("Playlist.Add"));

  /* Non-playlist playback: a player is active but reports playlistid -1. */
  stub_kodi_set_response ("Player.GetActivePlayers",
                          "[ { \"playerid\": 1, \"type\": \"video\" } ]");
  stub_kodi_set_response ("Player.GetProperties", "{ \"playlistid\": -1 }");

  g_autoptr (JsonNode) res2 =
    mk_tools_call (tools, "queue", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);
  is_error = FALSE;
  g_autoptr (JsonNode) payload2 = envelope_payload (res2, &is_error);
  MK_CHECK (is_error);
  MK_CHECK (!called ("Playlist.Add"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* Argument validation: exactly one of id/file, and id needs a known type —
 * each rejected before any Kodi call. */
static void
case_queue_bad_args (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  static const char *const bad[] = {
    "{}",                                          /* neither id nor file */
    "{ \"id\": 5, \"file\": \"/v/a.mkv\" }",       /* both */
    "{ \"id\": 5 }",                               /* id without type */
    "{ \"id\": 5, \"type\": \"album\" }",          /* unknown type */
  };
  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_autoptr (JsonNode) an = args_node (bad[i]);
      GError *error = NULL;
      g_autoptr (JsonNode) res =
        mk_tools_call (tools, "queue", json_node_get_object (an), &error);

      MK_CHECK (error == NULL);
      gboolean is_error = FALSE;
      g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
      (void) payload;
      MK_CHECK (is_error);
      g_clear_error (&error);
    }

  /* none of them reached Kodi. */
  MK_CHECK (!called ("Player.GetActivePlayers"));

  free_tools (tools, cfg, kodi);
}

/* ---- transport button (play) ---------------------------------------------- */

static void
case_button_play (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response ("Input.ExecuteAction", "\"OK\"");
  stub_kodi_set_response ("Player.GetActivePlayers",
                          "[ { \"playerid\": 0, \"type\": \"audio\" } ]");
  stub_kodi_set_response ("Player.GetProperties", "{ \"speed\": 1 }");
  stub_kodi_set_response (
    "Player.GetItem",
    "{ \"item\": { \"title\": \"Song\", \"file\": \"/s/a.flac\","
    "  \"type\": \"song\", \"id\": 5 } }");

  GError *error = NULL;
  g_autoptr (JsonNode) res = mk_tools_call (tools, "play", NULL, &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "state"), "playing");
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "type"), "audio");
    }

  /* Input.ExecuteAction then the snapshot. */
  MK_CHECK (called ("Input.ExecuteAction"));
  MK_CHECK (called ("Player.GetActivePlayers"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* ---- mute / unmute --------------------------------------------------------- */

static void
case_mute (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response ("Application.SetMute", "true");
  stub_kodi_set_response ("Application.GetProperties",
                          "{ \"muted\": true, \"volume\": 55 }");

  GError *error = NULL;
  g_autoptr (JsonNode) res = mk_tools_call (tools, "mute", NULL, &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      MK_CHECK (json_object_get_boolean_member (p, "muted"));
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "volume"), 55);
    }

  /* Application.SetMute then app_audio_state()'s read-back. */
  MK_CHECK (called ("Application.SetMute"));
  MK_CHECK (called ("Application.GetProperties"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

static void
case_unmute (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response ("Application.SetMute", "false");
  stub_kodi_set_response ("Application.GetProperties",
                          "{ \"muted\": false, \"volume\": 40 }");

  GError *error = NULL;
  g_autoptr (JsonNode) res = mk_tools_call (tools, "unmute", NULL, &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      MK_CHECK (!json_object_get_boolean_member (p, "muted"));
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "volume"), 40);
    }

  MK_CHECK (called ("Application.SetMute"));
  MK_CHECK (called ("Application.GetProperties"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* ---- rpc gate -------------------------------------------------------------- */

static void
case_rpc_disabled (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  /* The default instance has allow_rpc = FALSE: the call must be refused
   * cleanly, BEFORE any Kodi request. */
  g_autoptr (JsonNode) an = args_node ("{ \"method\": \"GUI.ActivateWindow\" }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "rpc", json_node_get_object (an), &error);

  /* a handler failure is shaped as isError, not a NULL/protocol error. */
  MK_CHECK (error == NULL);
  MK_CHECK (res != NULL);

  gboolean is_error = FALSE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    MK_CHECK (json_object_has_member (json_node_get_object (payload), "error"));

  /* the gate refused it before reaching Kodi: zero RPCs recorded. */
  MK_CHECK_INT_EQ (stub_kodi_methods->len, 0);

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

static void
case_rpc_allowed (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  /* On the allow_rpc instance the raw Kodi result is returned verbatim. */
  stub_kodi_set_response ("Custom.Method", "{ \"answer\": 42 }");

  g_autoptr (JsonNode) an =
    args_node ("{ \"instance\": \"rpcbox\", \"method\": \"Custom.Method\" }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "rpc", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    MK_CHECK_INT_EQ (
      json_object_get_int_member (json_node_get_object (payload), "answer"), 42);

  MK_CHECK (called ("Custom.Method"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* ---- instances get (no Kodi call, no save) -------------------------------- */

static void
case_instances_get (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  g_autoptr (JsonNode) an = args_node ("{ \"action\": \"get\" }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "instances", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      MK_CHECK_STR_EQ (json_object_get_string_member (p, "default"), "default");
      MK_CHECK (json_object_has_member (p, "instances"));
    }

  /* a config read makes no Kodi call. */
  MK_CHECK_INT_EQ (stub_kodi_methods->len, 0);

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* ---- handler failure path -------------------------------------------------- */

static void
case_handler_failure_shaped (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  /* Make the very first call noop issues fail; the handler returns NULL with a
   * GError, which mk_tools_call must shape into an isError result rather than
   * propagate as a NULL/protocol error. */
  stub_kodi_set_error ("Player.GetActivePlayers");

  GError *error = NULL;
  g_autoptr (JsonNode) res = mk_tools_call (tools, "noop", NULL, &error);

  MK_CHECK (error == NULL); /* shaped into the envelope, not returned */
  MK_CHECK (res != NULL);

  gboolean is_error = FALSE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    MK_CHECK (json_object_has_member (json_node_get_object (payload), "error"));

  MK_CHECK (called ("Player.GetActivePlayers"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* ---- volume (relative Knob, §11.6.2.1) ------------------------------------ */

/* Number of recorded Kodi calls this case. */
static guint
n_calls (void)
{
  return stub_kodi_methods != NULL ? stub_kodi_methods->len : 0;
}

/* step 0 (here: omitted) reads the live state and writes nothing — one round
 * trip, no SetVolume, and the fixed bounds come back. */
static void
case_volume_read_only (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response ("Application.GetProperties",
                          "{ \"muted\": false, \"volume\": 30 }");

  GError *error = NULL;
  g_autoptr (JsonNode) res = mk_tools_call (tools, "volume", NULL, &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "volume"), 30);
      MK_CHECK (!json_object_get_boolean_member (p, "muted"));
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "min"), 0);
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "max"), 100);
    }

  /* read only: GetProperties and nothing else */
  MK_CHECK (called ("Application.GetProperties"));
  MK_CHECK (!called ("Application.SetVolume"));
  MK_CHECK_INT_EQ (n_calls (), 1);

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* A relative +N: read, set, and report the volume Kodi *settled* on — here the
 * box echoes 42 even though the naive target is 40, and the handler must report
 * 42, proving it trusts the setter's reply. muted is carried from the read
 * (SetVolume never returns it). Two round trips. */
static void
case_volume_relative_step (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response ("Application.GetProperties",
                          "{ \"muted\": true, \"volume\": 30 }");
  stub_kodi_set_response ("Application.SetVolume", "42");

  g_autoptr (JsonNode) an = args_node ("{ \"step\": 10 }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "volume", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    {
      JsonObject *p = json_node_get_object (payload);
      MK_CHECK_INT_EQ (json_object_get_int_member (p, "volume"), 42);
      /* muted reused from the initial read, not from SetVolume */
      MK_CHECK (json_object_get_boolean_member (p, "muted"));
    }

  /* read then write: two round trips */
  MK_CHECK (called ("Application.GetProperties"));
  MK_CHECK (called ("Application.SetVolume"));
  MK_CHECK_INT_EQ (n_calls (), 2);

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

/* A step past the ceiling clamps to 100: from 95, +20 must target 100, not 115.
 * SetVolume returns a non-integer here, so the handler falls back to its OWN
 * computed target — which is exactly the clamp we want to verify. */
static void
case_volume_clamps_high (void)
{
  MkConfig *cfg;
  MkKodi *kodi;
  MkTools *tools = make_tools (&cfg, &kodi);

  stub_kodi_set_response ("Application.GetProperties",
                          "{ \"muted\": false, \"volume\": 95 }");
  stub_kodi_set_response ("Application.SetVolume", "{}"); /* non-int → use target */

  g_autoptr (JsonNode) an = args_node ("{ \"step\": 20 }");
  GError *error = NULL;
  g_autoptr (JsonNode) res =
    mk_tools_call (tools, "volume", json_node_get_object (an), &error);

  MK_CHECK (error == NULL);

  gboolean is_error = TRUE;
  g_autoptr (JsonNode) payload = envelope_payload (res, &is_error);
  MK_CHECK (!is_error);
  if (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload))
    MK_CHECK_INT_EQ (
      json_object_get_int_member (json_node_get_object (payload), "volume"),
      100);

  MK_CHECK (called ("Application.SetVolume"));

  g_clear_error (&error);
  free_tools (tools, cfg, kodi);
}

int
main (int argc, char **argv)
{
  static const MkTestCase cases[] = {
    { "search-movie",            case_search_movie },
    { "search-music",            case_search_music },
    { "playfile",                case_playfile },
    { "queue-append",            case_queue_append },
    { "queue-next-inserts",      case_queue_next_inserts },
    { "queue-nothing-playing",   case_queue_nothing_playing },
    { "queue-bad-args",          case_queue_bad_args },
    { "button-play",             case_button_play },
    { "mute",                    case_mute },
    { "unmute",                  case_unmute },
    { "volume-read-only",        case_volume_read_only },
    { "volume-relative-step",    case_volume_relative_step },
    { "volume-clamps-high",      case_volume_clamps_high },
    { "rpc-disabled",            case_rpc_disabled },
    { "rpc-allowed",             case_rpc_allowed },
    { "instances-get",           case_instances_get },
    { "handler-failure-shaped",  case_handler_failure_shaped },
  };
  return mk_test_run (argc, argv, cases, G_N_ELEMENTS (cases));
}
