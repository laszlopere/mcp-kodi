/* mcp-kodi — test stub for the Kodi JSON-RPC client (mk-kodi).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Drop-in replacement for src/mk-kodi.c that performs NO network I/O: it links
 * no libsoup and opens no socket. mk_kodi_call() returns canned JSON-RPC
 * results keyed by method name, so tool handlers can be exercised end-to-end
 * with deterministic input and zero traffic. Every call is recorded in
 * stub_kodi_methods so a test can assert which RPCs a handler issued.
 *
 * Implements the full mk-kodi.h surface so it satisfies the linker in place of
 * the real client.
 */

#include "mk-kodi.h"

#include <string.h>

/* ---- Call recording, inspected by the tests ------------------------------ */

/* Names of every method passed to mk_kodi_call(), in order. Owned strings. */
GPtrArray *stub_kodi_methods = NULL;

/* Reset the recorded call log between cases. */
void
stub_kodi_reset (void)
{
  if (stub_kodi_methods != NULL)
    g_ptr_array_unref (stub_kodi_methods);
  stub_kodi_methods = g_ptr_array_new_with_free_func (g_free);
}

/* ---- The stubbed client -------------------------------------------------- */

struct _MkKodi
{
  int unused;
};

G_DEFINE_QUARK (mk-kodi-error-quark, mk_kodi_error)

MkKodi *
mk_kodi_new (MkConfig *config)
{
  (void) config;
  return g_new0 (MkKodi, 1);
}

void
mk_kodi_free (MkKodi *self)
{
  g_free (self);
}

/* Parse a JSON literal into a node, aborting the test on a malformed literal
 * (that would be a bug in the stub, not the code under test). */
static JsonNode *
canned (const char *json)
{
  GError *error = NULL;
  JsonNode *node = json_from_string (json, &error);
  g_assert_no_error (error);
  return node;
}

/**
 * mk_kodi_call:
 *
 * Canned, traffic-free stand-in for the real call. Records @method and returns
 * a fixed result shaped like Kodi's so player_state() and friends parse it:
 *   - Player.GetActivePlayers → one audio player.
 *   - Player.GetProperties    → speed 1 with a time/totaltime.
 *   - Player.GetItem          → a song item with title/file/type/id.
 *   - anything else           → an empty object.
 * Never fails, so @error is left untouched and NULL is never returned.
 */
JsonNode *
mk_kodi_call (MkKodi     *self,
              const char *instance,
              const char *method,
              JsonNode   *params,
              GError    **error)
{
  (void) self;
  (void) instance;
  (void) params;
  (void) error;

  if (stub_kodi_methods == NULL)
    stub_kodi_reset ();
  g_ptr_array_add (stub_kodi_methods, g_strdup (method));

  if (strcmp (method, "Player.GetActivePlayers") == 0)
    return canned ("[ { \"playerid\": 0, \"type\": \"audio\" } ]");

  if (strcmp (method, "Player.GetProperties") == 0)
    return canned ("{ \"speed\": 1,"
                   "  \"time\": { \"hours\": 0, \"minutes\": 1, "
                   "\"seconds\": 12, \"milliseconds\": 0 },"
                   "  \"totaltime\": { \"hours\": 0, \"minutes\": 3, "
                   "\"seconds\": 30, \"milliseconds\": 0 } }");

  if (strcmp (method, "Player.GetItem") == 0)
    return canned ("{ \"item\": { \"title\": \"Test Track\","
                   "  \"file\": \"/music/test.flac\","
                   "  \"label\": \"Test Track\","
                   "  \"type\": \"song\", \"id\": 42 } }");

  return canned ("{}");
}
