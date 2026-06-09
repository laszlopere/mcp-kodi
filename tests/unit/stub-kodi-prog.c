/* mcp-kodi — programmable test stub for the Kodi JSON-RPC client (mk-kodi).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * A more flexible sibling of stub-kodi.c: a drop-in replacement for src/mk-kodi.c
 * that performs NO network I/O (no libsoup, no socket), but whose mk_kodi_call()
 * is *programmable per JSON-RPC method*. A test sets the canned `result` for a
 * method with stub_kodi_set_response(), or makes a method fail (NULL + GError)
 * with stub_kodi_set_error(), so a handler that issues several different calls —
 * `search` resolving an artist then fetching its songs, or `playfile` opening
 * then snapshotting — can be driven end-to-end with realistic, per-call replies.
 * Every call is recorded in stub_kodi_methods (in order) so a test can assert
 * exactly which RPCs a handler routed through.
 *
 * Implements the full mk-kodi.h surface so it satisfies the linker in place of
 * the real client.
 */

#include "mk-kodi.h"

/* ---- Programmable state, driven by the tests ----------------------------- */

/* Names of every method passed to mk_kodi_call(), in order. Owned strings. */
GPtrArray *stub_kodi_methods = NULL;

/* method name -> canned JSON `result` literal (owned strings, both). */
static GHashTable *stub_kodi_responses = NULL;
/* method name -> present means "fail this call" (set; key owned). */
static GHashTable *stub_kodi_failures = NULL;

/* Reset the recorded call log and all programmed responses between cases. */
void
stub_kodi_reset (void)
{
  if (stub_kodi_methods != NULL)
    g_ptr_array_unref (stub_kodi_methods);
  stub_kodi_methods = g_ptr_array_new_with_free_func (g_free);

  if (stub_kodi_responses != NULL)
    g_hash_table_unref (stub_kodi_responses);
  stub_kodi_responses =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  if (stub_kodi_failures != NULL)
    g_hash_table_unref (stub_kodi_failures);
  stub_kodi_failures =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

/* Program the `result` mk_kodi_call() returns for METHOD: a JSON literal shaped
 * exactly like Kodi's `result` member (an object, array, …). */
void
stub_kodi_set_response (const char *method, const char *json_literal)
{
  if (stub_kodi_responses == NULL)
    stub_kodi_reset ();
  g_hash_table_insert (stub_kodi_responses, g_strdup (method),
                       g_strdup (json_literal));
}

/* Program METHOD to fail: mk_kodi_call() returns NULL with an MK_KODI_ERROR set,
 * exercising the handlers' failure-shaping path. */
void
stub_kodi_set_error (const char *method)
{
  if (stub_kodi_failures == NULL)
    stub_kodi_reset ();
  g_hash_table_add (stub_kodi_failures, g_strdup (method));
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
 * (that would be a bug in the test/stub, not the code under test). */
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
 * Traffic-free, programmable stand-in for the real call. Records @method, then:
 *   - if the test programmed @method to fail, returns NULL with @error set;
 *   - else if the test set a canned response for @method, returns a parsed copy;
 *   - else returns an empty object, so an unprogrammed probe (e.g. an absent
 *     Player.GetActivePlayers) reads as a benign "nothing there".
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

  if (stub_kodi_methods == NULL)
    stub_kodi_reset ();
  g_ptr_array_add (stub_kodi_methods, g_strdup (method));

  if (stub_kodi_failures != NULL
      && g_hash_table_contains (stub_kodi_failures, method))
    {
      g_set_error (error, MK_KODI_ERROR, MK_KODI_ERROR_RPC,
                   "stub: programmed failure for method %s", method);
      return NULL;
    }

  const char *json = stub_kodi_responses != NULL
                       ? g_hash_table_lookup (stub_kodi_responses, method)
                       : NULL;
  return canned (json != NULL ? json : "{}");
}
