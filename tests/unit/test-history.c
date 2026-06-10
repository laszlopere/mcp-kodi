/* mcp-kodi — unit tests for the playback-history module (mk-history).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Unlike test-tools.c — which links a *stub* history that does no I/O — this
 * suite drives the REAL module (src/mk-history.c) against a private temp file,
 * so it exercises the actual write/read/dedup/trim/window file logic. Every
 * case mints its own throwaway directory via g_dir_make_tmp(), backs the log
 * with an explicit path under it (never NULL → never the user's real state
 * dir), and tears it down afterwards. No network, no shared state.
 *
 * Covered:
 *   - record one snapshot then read it back with the expected fields;
 *   - dedup: re-recording an instance's last item is skipped;
 *   - "nothing loaded" snapshots (stopped, or no file/id) write nothing;
 *   - newest-first ordering across two distinct items;
 *   - cross-instance: NULL reads every box, a key filters to one;
 *   - a missing file reads as zero entries (not an error);
 *   - limit caps the result while total reports the full match count;
 *   - since/until window filtering, plus a malformed bound as a hard error;
 *   - merge (11.9.4): a re-sighting of the instance's newest entry merges
 *     (earliest `at` kept, `last_seen` added) however old it is; an
 *     out-of-order sighting of a non-newest entry merges within the window
 *     but is a distinct play beyond it; a new entry is inserted by its
 *     observation `at`, not blindly prepended.
 */

#define MK_TEST_IMPL
#include "mk-test.h"

#include "mk-history.h"

#include <glib/gstdio.h>

/* Mint a fresh temp dir and return the history.json path under it; the dir is
 * handed back via @out_dir so the case can clean it up. Both are owned by the
 * caller (g_free). */
static char *
make_history_path (char **out_dir)
{
  GError *err = NULL;
  char *dir = g_dir_make_tmp ("mk-history-test-XXXXXX", &err);
  MK_CHECK (dir != NULL);
  g_clear_error (&err);
  *out_dir = dir;
  return g_build_filename (dir ? dir : ".", "history.json", NULL);
}

/* Remove everything the log may have left (history.json + its .lock + .XXXXXX
 * temps), then the dir itself. Frees @dir and @path. */
static void
cleanup (char *dir, char *path)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  if (d != NULL)
    {
      const char *name;
      while ((name = g_dir_read_name (d)) != NULL)
        {
          char *f = g_build_filename (dir, name, NULL);
          g_unlink (f);
          g_free (f);
        }
      g_dir_close (d);
    }
  g_rmdir (dir);
  g_free (dir);
  g_free (path);
}

/* Parse a JSON object literal into a snapshot node; checks it parsed. */
static JsonNode *
snap (const char *json)
{
  GError *err = NULL;
  JsonNode *n = json_from_string (json, &err);
  MK_CHECK (n != NULL);
  g_clear_error (&err);
  return n;
}

/* The read array's length, NULL-safe. */
static guint
arr_len (JsonNode *node)
{
  if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
    return 0;
  return json_array_get_length (json_node_get_array (node));
}

/* The i-th entry object of a read array, or NULL. */
static JsonObject *
arr_obj (JsonNode *node, guint i)
{
  if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
    return NULL;
  JsonArray *a = json_node_get_array (node);
  if (i >= json_array_get_length (a))
    return NULL;
  return json_array_get_object_element (a, i);
}

/* ISO-8601 UTC stamp @offset_secs from now; caller frees. */
static char *
stamp (gint offset_secs)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
  g_autoptr (GDateTime) dt = g_date_time_add_seconds (now, offset_secs);
  return g_date_time_format_iso8601 (dt);
}

/* Seed @path with a pre-built newest-first log (a JSON array literal), as a
 * prior writer would have left it. */
static void
seed (const char *path, const char *json)
{
  GError *err = NULL;
  MK_CHECK (g_file_set_contents (path, json, -1, &err));
  g_clear_error (&err);
}

static void
case_record_then_read (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  g_autoptr (JsonNode) s = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"media\": \"song\","
    " \"id\": 42, \"file\": \"/music/a.mp3\", \"title\": \"Song A\","
    " \"album\": \"Album A\", \"artist\": [\"Abba\"], \"track\": 3 }");

  MK_CHECK (mk_history_record (h, "living", "Living Room", s));

  gint64 total = -1;
  GError *err = NULL;
  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 0, &total, &err);
  MK_CHECK (err == NULL);
  MK_CHECK (out != NULL && JSON_NODE_HOLDS_ARRAY (out));
  MK_CHECK_INT_EQ (arr_len (out), 1);
  MK_CHECK_INT_EQ (total, 1);

  JsonObject *e = arr_obj (out, 0);
  MK_CHECK (e != NULL);
  if (e != NULL)
    {
      /* the snapshot's `type` is recorded under `kind` */
      MK_CHECK_STR_EQ (json_object_get_string_member (e, "kind"), "audio");
      MK_CHECK_STR_EQ (json_object_get_string_member (e, "media"), "song");
      MK_CHECK_INT_EQ (json_object_get_int_member (e, "id"), 42);
      MK_CHECK_STR_EQ (json_object_get_string_member (e, "instance"), "living");
      MK_CHECK_STR_EQ (json_object_get_string_member (e, "name"),
                       "Living Room");
      MK_CHECK_STR_EQ (json_object_get_string_member (e, "file"),
                       "/music/a.mp3");
      MK_CHECK_STR_EQ (json_object_get_string_member (e, "title"), "Song A");
      MK_CHECK_STR_EQ (json_object_get_string_member (e, "album"), "Album A");
      MK_CHECK_INT_EQ (json_object_get_int_member (e, "track"), 3);
      /* an `at` stamp is always written */
      MK_CHECK (json_object_has_member (e, "at"));
      /* state/time/totaltime are deliberately NOT stored */
      MK_CHECK (!json_object_has_member (e, "state"));
    }

  g_clear_error (&err);
  cleanup (dir, path);
}

static void
case_dedup_same_item (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  g_autoptr (JsonNode) s1 = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 1,"
    " \"file\": \"/music/x.mp3\", \"title\": \"X\" }");
  g_autoptr (JsonNode) s2 = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 1,"
    " \"file\": \"/music/x.mp3\", \"title\": \"X\" }");

  MK_CHECK (mk_history_record (h, "box", "Box", s1));
  /* the same item, same instance, re-observed → skipped */
  MK_CHECK (!mk_history_record (h, "box", "Box", s2));

  gint64 total = -1;
  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 0, &total, NULL);
  MK_CHECK_INT_EQ (arr_len (out), 1);
  MK_CHECK_INT_EQ (total, 1);

  cleanup (dir, path);
}

static void
case_nothing_loaded_skips (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  /* stopped → nothing playing, nothing to record */
  g_autoptr (JsonNode) stopped = snap (
    "{ \"state\": \"stopped\", \"type\": \"audio\","
    " \"file\": \"/music/y.mp3\", \"id\": 5 }");
  MK_CHECK (!mk_history_record (h, "box", "Box", stopped));

  /* loaded but nothing identifies the item (no file, no id) → can't log */
  g_autoptr (JsonNode) anon = snap (
    "{ \"state\": \"playing\", \"type\": \"video\", \"title\": \"Nameless\" }");
  MK_CHECK (!mk_history_record (h, "box", "Box", anon));

  /* an empty-string file with no id is also "nothing to dedup on" */
  g_autoptr (JsonNode) blank = snap (
    "{ \"state\": \"playing\", \"type\": \"video\", \"file\": \"\" }");
  MK_CHECK (!mk_history_record (h, "box", "Box", blank));

  gint64 total = -1;
  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 0, &total, NULL);
  MK_CHECK_INT_EQ (arr_len (out), 0);
  MK_CHECK_INT_EQ (total, 0);

  cleanup (dir, path);
}

static void
case_newest_first (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  g_autoptr (JsonNode) a = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 1,"
    " \"file\": \"/a\", \"title\": \"A\" }");
  g_autoptr (JsonNode) b = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 2,"
    " \"file\": \"/b\", \"title\": \"B\" }");

  MK_CHECK (mk_history_record (h, "box", "Box", a));
  MK_CHECK (mk_history_record (h, "box", "Box", b));

  gint64 total = -1;
  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 0, &total, NULL);
  MK_CHECK_INT_EQ (arr_len (out), 2);
  MK_CHECK_INT_EQ (total, 2);
  /* most recently recorded item comes first */
  MK_CHECK_STR_EQ (json_object_get_string_member (arr_obj (out, 0), "title"),
                   "B");
  MK_CHECK_STR_EQ (json_object_get_string_member (arr_obj (out, 1), "title"),
                   "A");

  cleanup (dir, path);
}

static void
case_cross_instance (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  g_autoptr (JsonNode) s1 = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 1,"
    " \"file\": \"/one\", \"title\": \"One\" }");
  g_autoptr (JsonNode) s2 = snap (
    "{ \"state\": \"playing\", \"type\": \"video\", \"id\": 2,"
    " \"file\": \"/two\", \"title\": \"Two\" }");

  MK_CHECK (mk_history_record (h, "box1", "Box One", s1));
  MK_CHECK (mk_history_record (h, "box2", "Box Two", s2));

  /* NULL → the whole cross-instance log */
  gint64 total = -1;
  g_autoptr (JsonNode) all =
    mk_history_read (h, NULL, NULL, NULL, 0, &total, NULL);
  MK_CHECK_INT_EQ (arr_len (all), 2);
  MK_CHECK_INT_EQ (total, 2);

  /* a key restricts to that box */
  gint64 t1 = -1;
  g_autoptr (JsonNode) only1 =
    mk_history_read (h, "box1", NULL, NULL, 0, &t1, NULL);
  MK_CHECK_INT_EQ (arr_len (only1), 1);
  MK_CHECK_INT_EQ (t1, 1);
  MK_CHECK_STR_EQ (
    json_object_get_string_member (arr_obj (only1, 0), "instance"), "box1");

  cleanup (dir, path);
}

static void
case_missing_file_is_empty (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  /* never written → no file → zero entries, no error (not a failure) */
  MK_CHECK (!g_file_test (path, G_FILE_TEST_EXISTS));

  gint64 total = -1;
  GError *err = NULL;
  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 0, &total, &err);
  MK_CHECK (err == NULL);
  MK_CHECK (out != NULL && JSON_NODE_HOLDS_ARRAY (out));
  MK_CHECK_INT_EQ (arr_len (out), 0);
  MK_CHECK_INT_EQ (total, 0);

  g_clear_error (&err);
  cleanup (dir, path);
}

static void
case_limit_and_total (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  for (int i = 0; i < 4; i++)
    {
      char *json = g_strdup_printf (
        "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": %d,"
        " \"file\": \"/f%d\", \"title\": \"T%d\" }",
        i, i, i);
      g_autoptr (JsonNode) s = snap (json);
      g_free (json);
      MK_CHECK (mk_history_record (h, "box", "Box", s));
    }

  /* limit caps the returned set; total still reports the full match count */
  gint64 total = -1;
  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 2, &total, NULL);
  MK_CHECK_INT_EQ (arr_len (out), 2);
  MK_CHECK_INT_EQ (total, 4);
  /* the kept ones are the newest (last recorded was /f3) */
  MK_CHECK_STR_EQ (json_object_get_string_member (arr_obj (out, 0), "file"),
                   "/f3");
  MK_CHECK_STR_EQ (json_object_get_string_member (arr_obj (out, 1), "file"),
                   "/f2");

  cleanup (dir, path);
}

static void
case_window_filtering (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  /* `at` is server wall-clock at record time (≈ now), so we probe with
   * windows we know to be wholly out of / in range rather than guessing it. */
  g_autoptr (JsonNode) s = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 1,"
    " \"file\": \"/w\", \"title\": \"W\" }");
  MK_CHECK (mk_history_record (h, "box", "Box", s));

  /* a window entirely in the past excludes the just-recorded entry */
  gint64 t_old = -1;
  g_autoptr (JsonNode) old = mk_history_read (
    h, NULL, NULL, "2000-01-01T00:00:00Z", 0, &t_old, NULL);
  MK_CHECK_INT_EQ (arr_len (old), 0);
  MK_CHECK_INT_EQ (t_old, 0);

  /* a window entirely in the future also excludes it */
  gint64 t_fut = -1;
  g_autoptr (JsonNode) fut = mk_history_read (
    h, NULL, "2100-01-01T00:00:00Z", NULL, 0, &t_fut, NULL);
  MK_CHECK_INT_EQ (arr_len (fut), 0);
  MK_CHECK_INT_EQ (t_fut, 0);

  /* a window that straddles now keeps it */
  gint64 t_in = -1;
  g_autoptr (JsonNode) in = mk_history_read (
    h, NULL, "2000-01-01T00:00:00Z", "2100-01-01T00:00:00Z", 0, &t_in, NULL);
  MK_CHECK_INT_EQ (arr_len (in), 1);
  MK_CHECK_INT_EQ (t_in, 1);

  /* a malformed bound is a hard error, unlike missing-file */
  GError *err = NULL;
  g_autoptr (JsonNode) bad =
    mk_history_read (h, NULL, "not-a-timestamp", NULL, 0, NULL, &err);
  MK_CHECK (bad == NULL);
  MK_CHECK (err != NULL);
  if (err != NULL)
    MK_CHECK (g_error_matches (err, MK_HISTORY_ERROR,
                               MK_HISTORY_ERROR_INVALID));
  g_clear_error (&err);

  cleanup (dir, path);
}

static void
case_merge_newest_sets_last_seen (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  /* The instance's NEWEST entry matches on item identity alone, however old:
   * a later snapshot of the same continuous playback is the same play. */
  g_autofree char *old_at = stamp (-3600);
  g_autofree char *json = g_strdup_printf (
    "[ { \"at\": \"%s\", \"instance\": \"box\", \"kind\": \"audio\","
    "    \"id\": 1, \"file\": \"/x\", \"title\": \"X\" } ]",
    old_at);
  seed (path, json);

  g_autoptr (JsonNode) s = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 1,"
    " \"file\": \"/x\", \"title\": \"X\" }");
  /* merged into the existing entry, not appended */
  MK_CHECK (!mk_history_record (h, "box", "Box", s));

  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 0, NULL, NULL);
  MK_CHECK_INT_EQ (arr_len (out), 1);
  JsonObject *e = arr_obj (out, 0);
  MK_CHECK (e != NULL);
  if (e != NULL)
    {
      /* `at` keeps the earliest sighting; `last_seen` records the latest */
      MK_CHECK_STR_EQ (json_object_get_string_member (e, "at"), old_at);
      MK_CHECK (json_object_has_member (e, "last_seen"));
      MK_CHECK (g_strcmp0 (json_object_get_string_member_with_default (
                             e, "last_seen", ""),
                           old_at)
                > 0);
    }

  cleanup (dir, path);
}

static void
case_out_of_order_sighting_merges (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  /* Another writer already logged X then Y; our sighting of X arrives late.
   * X is no longer the newest entry, but it is within the merge window — the
   * sighting must fold into it, not spawn a second X (the 11.9.4 scenario). */
  g_autofree char *y_at = stamp (-30);
  g_autofree char *x_at = stamp (-120);
  g_autofree char *json = g_strdup_printf (
    "[ { \"at\": \"%s\", \"instance\": \"box\", \"kind\": \"audio\","
    "    \"id\": 2, \"file\": \"/y\", \"title\": \"Y\" },"
    "  { \"at\": \"%s\", \"instance\": \"box\", \"kind\": \"audio\","
    "    \"id\": 1, \"file\": \"/x\", \"title\": \"X\" } ]",
    y_at, x_at);
  seed (path, json);

  g_autoptr (JsonNode) s = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 1,"
    " \"file\": \"/x\", \"title\": \"X\" }");
  MK_CHECK (!mk_history_record (h, "box", "Box", s));

  gint64 total = -1;
  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 0, &total, NULL);
  MK_CHECK_INT_EQ (arr_len (out), 2); /* no spurious second X */
  MK_CHECK_INT_EQ (total, 2);
  JsonObject *x = arr_obj (out, 1);
  MK_CHECK (x != NULL);
  if (x != NULL)
    {
      /* X stayed below Y (ordered by `at`), kept its first-sighting stamp,
       * and the late sighting shows up as `last_seen` */
      MK_CHECK_STR_EQ (json_object_get_string_member (x, "file"), "/x");
      MK_CHECK_STR_EQ (json_object_get_string_member (x, "at"), x_at);
      MK_CHECK (json_object_has_member (x, "last_seen"));
    }

  cleanup (dir, path);
}

static void
case_replay_beyond_window_is_new (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  /* X played two hours ago, then Y. Playing X again now is a genuinely
   * distinct play: a non-newest entry beyond the merge window must NOT
   * absorb it. */
  g_autofree char *y_at = stamp (-60);
  g_autofree char *x_at = stamp (-7200);
  g_autofree char *json = g_strdup_printf (
    "[ { \"at\": \"%s\", \"instance\": \"box\", \"kind\": \"audio\","
    "    \"id\": 2, \"file\": \"/y\", \"title\": \"Y\" },"
    "  { \"at\": \"%s\", \"instance\": \"box\", \"kind\": \"audio\","
    "    \"id\": 1, \"file\": \"/x\", \"title\": \"X\" } ]",
    y_at, x_at);
  seed (path, json);

  g_autoptr (JsonNode) s = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 1,"
    " \"file\": \"/x\", \"title\": \"X\" }");
  MK_CHECK (mk_history_record (h, "box", "Box", s)); /* a NEW entry */

  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 0, NULL, NULL);
  MK_CHECK_INT_EQ (arr_len (out), 3);
  /* the fresh play lands on top; the old X keeps its own entry */
  MK_CHECK_STR_EQ (json_object_get_string_member (arr_obj (out, 0), "file"),
                   "/x");
  MK_CHECK (!json_object_has_member (arr_obj (out, 0), "last_seen"));
  MK_CHECK_STR_EQ (json_object_get_string_member (arr_obj (out, 2), "at"),
                   x_at);

  cleanup (dir, path);
}

static void
case_insert_by_observation_time (void)
{
  char *dir, *path = make_history_path (&dir);
  g_autoptr (MkHistory) h = mk_history_new (path);

  /* An entry stamped later than our observation (another writer's newer
   * sighting already landed): the new entry must be inserted by its `at`,
   * below it — not blindly prepended on top. */
  g_autofree char *z_at = stamp (3600);
  g_autofree char *json = g_strdup_printf (
    "[ { \"at\": \"%s\", \"instance\": \"box\", \"kind\": \"audio\","
    "    \"id\": 9, \"file\": \"/z\", \"title\": \"Z\" } ]",
    z_at);
  seed (path, json);

  g_autoptr (JsonNode) s = snap (
    "{ \"state\": \"playing\", \"type\": \"audio\", \"id\": 1,"
    " \"file\": \"/x\", \"title\": \"X\" }");
  MK_CHECK (mk_history_record (h, "box", "Box", s));

  g_autoptr (JsonNode) out =
    mk_history_read (h, NULL, NULL, NULL, 0, NULL, NULL);
  MK_CHECK_INT_EQ (arr_len (out), 2);
  MK_CHECK_STR_EQ (json_object_get_string_member (arr_obj (out, 0), "file"),
                   "/z");
  MK_CHECK_STR_EQ (json_object_get_string_member (arr_obj (out, 1), "file"),
                   "/x");

  cleanup (dir, path);
}

int
main (int argc, char **argv)
{
  static const MkTestCase cases[] = {
    { "record-then-read",     case_record_then_read },
    { "dedup-same-item",      case_dedup_same_item },
    { "nothing-loaded-skips", case_nothing_loaded_skips },
    { "newest-first",         case_newest_first },
    { "cross-instance",       case_cross_instance },
    { "missing-file-empty",   case_missing_file_is_empty },
    { "limit-and-total",      case_limit_and_total },
    { "window-filtering",     case_window_filtering },
    { "merge-newest-last-seen",   case_merge_newest_sets_last_seen },
    { "merge-out-of-order",       case_out_of_order_sighting_merges },
    { "replay-beyond-window-new", case_replay_beyond_window_is_new },
    { "insert-by-at",             case_insert_by_observation_time },
  };
  return mk_test_run (argc, argv, cases, G_N_ELEMENTS (cases));
}
