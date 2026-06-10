/* mcp-kodi — playback history log (skeleton). See mk-history.h.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 */

#include "config.h"

#include "mk-history.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <glib/gstdio.h>

/* Retention bounds, tuned once we see real volume. With sparse,
 * call-driven entries 10k rows is many months — comfortably past
 * "last month". */
#define MK_HISTORY_MAX          10000 /* keep newest this many entries */
#define MK_HISTORY_MAX_AGE_DAYS 180   /* drop entries older than this  */

/* How close (in either direction) an observation must land to an entry's
 * [at, last_seen] coverage to merge into it when that entry is NOT the
 * instance's newest — i.e. when something else played after it. Such a merge
 * is only right for a late or out-of-order sighting of the *same* play by
 * another process (11.9.5: several pollers, flock-serialized, observing the
 * same boxes), and those arrive seconds late, not hours — while a genuine
 * replay of an item after something else must stay a distinct play. Ten
 * minutes absorbs lock contention, clock skew and a few missed ~2-minute
 * polls with room to spare. The instance's *newest* entry keeps matching
 * with no window at all, preserving the call-driven semantics: a snapshot of
 * the same continuous playback hours later is still the same play. */
#define MK_HISTORY_MERGE_WINDOW (10 * G_TIME_SPAN_MINUTE)

/* Coarsening step for the `last_seen` bump on the instance's NEWEST entry.
 * While one item plays continuously, every ~2-minute poll re-sights it and
 * would rewrite the whole log just to nudge `last_seen` forward — a long
 * single play (a movie) could rewrite the file dozens of times saying nothing
 * new. So a bare advance of the newest entry's `last_seen` is persisted only
 * once it clears this much past what is stored: the file then updates in steps,
 * not once per poll, and a hard exit loses at most this much `last_seen`
 * resolution. Kept at the merge window so `last_seen` is never staler than that
 * window already tolerates when matching late, out-of-order sightings. Only the
 * newest (continuous-playback) merge is coarsened; a rare out-of-order sighting
 * folding into an OLDER entry still records its `last_seen` precisely, since it
 * is not a per-poll amplification source and its freshness aids cross-process
 * convergence. */
#define MK_HISTORY_LAST_SEEN_STEP (10 * G_TIME_SPAN_MINUTE)

struct _MkHistory
{
  char *path; /* resolved path to the one global history.json (owned) */
};

/**
 * mk_history_error_quark:
 *
 * Defined by G_DEFINE_QUARK().
 *
 * @return the GQuark identifying the MK_HISTORY_ERROR domain.
 */
G_DEFINE_QUARK (mk-history-error-quark, mk_history_error)

/**
 * mk_history_default_path:
 *
 * Returns the default history path,
 * ${XDG_STATE_HOME:-~/.local/state}/mcp-kodi/history.json, computed once
 * and cached. This is state, not config — a backward-looking log of what played
 * — so it lives under XDG_STATE_HOME, separate from the config file and the
 * per-instance state files.
 *
 * @return the path string, owned by the library; do not free.
 */
const char *
mk_history_default_path (void)
{
  static char *path = NULL;
  if (g_once_init_enter (&path))
    {
      /* g_get_user_state_dir() honours XDG_STATE_HOME and falls back to
       * ~/.local/state. */
      char *p = g_build_filename (g_get_user_state_dir (),
                                  "mcp-kodi", "history.json", NULL);
      g_once_init_leave (&path, p);
    }
  return path;
}

/**
 * mk_history_new:
 * @path: path to the history log file, or NULL for mk_history_default_path().
 *
 * Allocates a history log bound to @path. Does not touch the filesystem: the
 * directory and file are created lazily by the first write, once the
 * write path lands.
 *
 * @return a newly allocated MkHistory; free with mk_history_free().
 */
MkHistory *
mk_history_new (const char *path)
{
  MkHistory *self = g_new0 (MkHistory, 1);
  self->path = g_strdup (path ? path : mk_history_default_path ());
  return self;
}

/**
 * mk_history_free:
 * @self: the history log to free, or NULL.
 *
 * Frees the log and its owned path. Safe to call with NULL.
 */
void
mk_history_free (MkHistory *self)
{
  if (self == NULL)
    return;
  g_free (self->path);
  g_free (self);
}

/**
 * mk_history_path:
 * @self: the history log.
 *
 * @return the resolved path of the log file, owned by @self; do not free.
 */
const char *
mk_history_path (MkHistory *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->path;
}

/* ---- write path ---------------------------------------------------------- */

/**
 * write_all:
 * @fd: an open, writable file descriptor.
 * @data: the bytes to write.
 * @len: how many bytes.
 *
 * Writes the whole buffer, retrying short writes and EINTR.
 *
 * @return TRUE on success; FALSE with errno set on a write error.
 */
static gboolean
write_all (int fd, const char *data, gsize len)
{
  gsize off = 0;
  while (off < len)
    {
      gssize w = write (fd, data + off, len - off);
      if (w < 0)
        {
          if (errno == EINTR)
            continue;
          return FALSE;
        }
      off += (gsize) w;
    }
  return TRUE;
}

/**
 * hist_copy:
 * @b: the builder, positioned inside an object.
 * @src: the snapshot object to copy from.
 * @name: the member to copy verbatim if present.
 *
 * Copies @src[@name] into the entry under the same key, or does nothing when
 * absent. The snapshot has already dropped the per-media fields that don't
 * apply (copy_member_nonempty in player_state()), so "omit what's empty"
 * needs no further filtering here.
 */
static void
hist_copy (JsonBuilder *b, JsonObject *src, const char *name)
{
  if (!json_object_has_member (src, name))
    return;
  json_builder_set_member_name (b, name);
  json_builder_add_value (b, json_node_copy (json_object_get_member (src, name)));
}

/**
 * hist_copy_as:
 * @b: the builder, positioned inside an object.
 * @src: the snapshot object to copy from.
 * @from: the snapshot member to read.
 * @to: the entry key to write it under.
 *
 * Like hist_copy() but renames the key — used to record the snapshot's `type`
 * (the player kind) under the entry's `kind`.
 */
static void
hist_copy_as (JsonBuilder *b, JsonObject *src, const char *from, const char *to)
{
  if (!json_object_has_member (src, from))
    return;
  json_builder_set_member_name (b, to);
  json_builder_add_value (b, json_node_copy (json_object_get_member (src, from)));
}

/**
 * compose_entry:
 * @snap: the now-playing snapshot (player_state()).
 * @instance: the instance config key.
 * @name: the instance display label, or NULL.
 * @at: the capture timestamp, ISO-8601 with timezone.
 *
 * Builds one history entry from the snapshot plus the instance
 * key/name and capture time: `at`/`instance`/`name`, the player kind as `kind`
 * (from the snapshot's `type`), then `media`/`id` and the per-media
 * identifiers copied straight across — present only where the
 * snapshot carries them. `state`/`time`/`totaltime` are deliberately not
 * stored: the entry asserts only "this was played at `at`".
 *
 * @return a newly allocated object node; free with json_node_unref().
 */
static JsonNode *
compose_entry (JsonObject *snap, const char *instance, const char *name,
               const char *at)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "at");
  json_builder_add_string_value (b, at);
  json_builder_set_member_name (b, "instance");
  json_builder_add_string_value (b, instance);
  if (name != NULL)
    {
      json_builder_set_member_name (b, "name");
      json_builder_add_string_value (b, name);
    }
  hist_copy_as (b, snap, "type", "kind"); /* player kind */
  hist_copy (b, snap, "media");           /* real media type */
  hist_copy (b, snap, "id");              /* library id */
  hist_copy (b, snap, "title");
  hist_copy (b, snap, "showtitle"); /* per-media identifiers from here on */
  hist_copy (b, snap, "season");
  hist_copy (b, snap, "episode");
  hist_copy (b, snap, "album");
  hist_copy (b, snap, "artist");
  hist_copy (b, snap, "track");
  hist_copy (b, snap, "file");
  hist_copy (b, snap, "label");
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

/**
 * same_item:
 * @e: an existing history entry.
 * @file: the snapshot's file path, or NULL.
 * @have_id: whether the snapshot carries a library id.
 * @id: that library id (valid only when @have_id).
 *
 * The item-identity test shared by all merge matching: same `file` when both
 * sides carry one, else same `id`.
 *
 * @return TRUE if @e describes the same item the snapshot does.
 */
static gboolean
same_item (JsonObject *e, const char *file, gboolean have_id, gint64 id)
{
  const char *efile =
    json_object_get_string_member_with_default (e, "file", NULL);
  if (file != NULL && efile != NULL)
    return g_strcmp0 (efile, file) == 0;
  return have_id && json_object_has_member (e, "id")
         && json_object_get_int_member_with_default (e, "id", -2) == id;
}

/**
 * entry_time:
 * @e: a history entry object.
 * @name: the member holding an ISO-8601 stamp ("at" or "last_seen").
 *
 * Parses @e[@name] as a timestamp.
 *
 * @return a new GDateTime, or NULL when the member is absent or unparseable;
 *         free with g_date_time_unref().
 */
static GDateTime *
entry_time (JsonObject *e, const char *name)
{
  const char *s = json_object_get_string_member_with_default (e, name, NULL);
  return s != NULL ? g_date_time_new_from_iso8601 (s, NULL) : NULL;
}

/**
 * find_merge_match:
 * @existing: the current entries, newest-first by `at`, or NULL.
 * @instance: the observing instance's config key.
 * @file: the snapshot's file path, or NULL.
 * @have_id: whether the snapshot carries a library id.
 * @id: that library id (valid only when @have_id).
 * @at: the observation timestamp.
 * @was_newest: out; set TRUE when the match is the instance's newest entry (a
 *        continuous-playback re-sighting), FALSE for an older window match.
 *        Untouched on a -1 (no-match) return; the caller coarsens only the
 *        newest case.
 *
 * Finds the entry an observation of (@instance, item, @at) merges into.
 * The instance's newest entry matches on item identity alone — re-observing
 * the same continuous playback, however much later, is the same play (the
 * pre-11.9.4 dedup rule, kept). Older entries of the instance match only when
 * @at also falls within MK_HISTORY_MERGE_WINDOW of their [at, last_seen]
 * coverage: that is another process's late or out-of-order sighting of a play
 * already logged (X arriving after X-then-Y must not spawn a second X), while
 * a repeat beyond the window stays a genuinely distinct play. An older entry
 * whose `at` is unparseable can't be placed in time and never window-matches.
 *
 * @return the index of the entry to merge into, or -1 to append a new entry.
 */
static gint
find_merge_match (JsonArray *existing, const char *instance, const char *file,
                  gboolean have_id, gint64 id, GDateTime *at,
                  gboolean *was_newest)
{
  if (existing == NULL)
    return -1;
  gboolean newest = TRUE; /* the next entry of @instance is its newest */
  guint n = json_array_get_length (existing);
  for (guint i = 0; i < n; i++)
    {
      JsonObject *e = json_array_get_object_element (existing, i);
      if (e == NULL)
        continue;
      if (g_strcmp0 (json_object_get_string_member_with_default (e, "instance",
                                                                 NULL),
                     instance)
          != 0)
        continue;
      gboolean is_newest = newest;
      newest = FALSE;
      if (!same_item (e, file, have_id, id))
        continue;
      if (is_newest)
        {
          if (was_newest != NULL)
            *was_newest = TRUE;
          return (gint) i;
        }
      if (was_newest != NULL)
        *was_newest = FALSE;
      /* an older entry: merge only a near-in-time re-sighting */
      g_autoptr (GDateTime) eat = entry_time (e, "at");
      if (eat == NULL)
        continue;
      g_autoptr (GDateTime) eseen = entry_time (e, "last_seen");
      GDateTime *upper = eseen != NULL ? eseen : eat;
      if (g_date_time_difference (eat, at) <= MK_HISTORY_MERGE_WINDOW
          && g_date_time_difference (at, upper) <= MK_HISTORY_MERGE_WINDOW)
        return (gint) i;
    }
  return -1;
}

/**
 * merge_entry:
 * @oldnode: the matched existing entry's node (borrowed; not modified).
 * @at: the incoming observation timestamp, ISO-8601.
 * @at_dt: the same, parsed.
 * @sort_key: out; a new ref on the merged entry's effective `at` for sorted
 *            re-insertion, or NULL when the old `at` is unparseable (the
 *            entry then keeps its current position).
 * @coarsen: when TRUE (the matched entry is the instance's newest, i.e. a
 *           continuous-playback re-sighting), a bare `last_seen` advance is
 *           persisted only once it clears MK_HISTORY_LAST_SEEN_STEP past what
 *           is stored, so a long single play does not rewrite the log every
 *           poll. FALSE (an older out-of-order match) records `last_seen`
 *           precisely, as before.
 * @changed: out; FALSE when the merge altered nothing (a same-second
 *           re-sighting, @at already inside the entry's coverage, or a
 *           coarsened sub-step advance) and the file need not be rewritten.
 *
 * Folds a re-sighting into the entry it matched: `at` keeps the *earliest*
 * sighting and `last_seen` the latest, so concurrent writers converge on one
 * entry no matter the arrival order. `last_seen` is written only once it
 * differs from `at` — absent means "seen once". All other fields stay as
 * first recorded (the sighting is of the same item, so they agree).
 *
 * @return a newly allocated merged entry node; free with json_node_unref().
 */
static JsonNode *
merge_entry (JsonNode *oldnode, const char *at, GDateTime *at_dt,
             gboolean coarsen, GDateTime **sort_key, gboolean *changed)
{
  JsonNode *node = json_node_copy (oldnode);
  JsonObject *o = json_node_get_object (node);

  const char *old_at =
    json_object_get_string_member_with_default (o, "at", NULL);
  const char *old_seen =
    json_object_get_string_member_with_default (o, "last_seen", NULL);
  g_autoptr (GDateTime) old_at_dt = entry_time (o, "at");
  g_autoptr (GDateTime) old_seen_dt = entry_time (o, "last_seen");

  /* the earliest sighting wins `at` */
  g_autofree char *new_at = NULL; /* NULL = keep the entry's `at` */
  if (old_at_dt != NULL && g_date_time_compare (at_dt, old_at_dt) < 0)
    new_at = g_strdup (at);

  /* the latest sighting wins `last_seen`; record it only when it adds
   * information (differs from the final `at` and from what is stored) */
  GDateTime *upper_dt = old_seen_dt != NULL ? old_seen_dt : old_at_dt;
  const char *upper = old_seen != NULL ? old_seen : old_at;
  const char *latest =
    (upper_dt == NULL || g_date_time_compare (at_dt, upper_dt) > 0) ? at
                                                                    : upper;
  const char *final_at = new_at != NULL ? new_at : old_at;
  g_autofree char *new_seen = NULL; /* NULL = keep the entry's `last_seen` */
  if (g_strcmp0 (latest, final_at) != 0 && g_strcmp0 (latest, old_seen) != 0)
    {
      /* The latest sighting advances `last_seen`. On the newest entry this
       * fires every poll while one item plays, rewriting the whole log just to
       * nudge the stamp; coarsen it — persist the bump only once it clears the
       * step past what is stored. When `at` itself is being rewritten earlier
       * (new_at set) the file is rewritten anyway, so record the precise
       * latest then; likewise when not coarsening (an older out-of-order
       * match) or when the stored upper is unparseable. */
      gint64 advance = upper_dt != NULL
                         ? g_date_time_difference (at_dt, upper_dt)
                         : G_MAXINT64;
      if (!coarsen || new_at != NULL || advance > MK_HISTORY_LAST_SEEN_STEP)
        new_seen = g_strdup (latest);
    }

  *changed = new_at != NULL || new_seen != NULL;
  if (new_at != NULL)
    json_object_set_string_member (o, "at", new_at);
  if (new_seen != NULL)
    json_object_set_string_member (o, "last_seen", new_seen);

  if (new_at != NULL)
    *sort_key = g_date_time_ref (at_dt);
  else
    *sort_key = old_at_dt != NULL ? g_date_time_ref (old_at_dt) : NULL;
  return node;
}

/**
 * atomic_write:
 * @path: the target file.
 * @data: the bytes to write.
 * @error: return location for a GError.
 *
 * Writes @data to a 0600 temp file in @path's directory, fsync()s it, then
 * rename()s it over @path — the usual atomic-replacement discipline,
 * run inside the caller's lock.
 *
 * @return TRUE on success; FALSE with @error set on I/O failure.
 */
static gboolean
atomic_write (const char *path, const char *data, GError **error)
{
  gsize len = strlen (data);
  g_autofree char *tmpl = g_strconcat (path, ".XXXXXX", NULL);
  int fd = g_mkstemp_full (tmpl, O_WRONLY, 0600);
  if (fd == -1)
    {
      g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_IO,
                   "create temp file near %s: %s", path, g_strerror (errno));
      return FALSE;
    }
  if (!write_all (fd, data, len) || fsync (fd) != 0)
    {
      g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_IO,
                   "write %s: %s", tmpl, g_strerror (errno));
      close (fd);
      g_unlink (tmpl);
      return FALSE;
    }
  if (close (fd) != 0)
    {
      g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_IO, "close %s: %s",
                   tmpl, g_strerror (errno));
      g_unlink (tmpl);
      return FALSE;
    }
  if (g_rename (tmpl, path) != 0)
    {
      g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_IO,
                   "rename onto %s: %s", path, g_strerror (errno));
      g_unlink (tmpl);
      return FALSE;
    }
  return TRUE;
}

/**
 * record_locked:
 * @self: the history log.
 * @snap: the now-playing snapshot, already known to be loaded.
 * @instance: the instance config key.
 * @name: the instance display label, or NULL.
 * @at: the observation timestamp, ISO-8601 UTC, captured pre-lock.
 * @at_dt: the same, parsed.
 * @error: return location for a GError.
 *
 * The critical section of mk_history_record(), run under the exclusive
 * lock: read the current array → match the observation against the
 * instance's recent entries (find_merge_match) → merge into the matched
 * entry or compose a new one → re-emit the array ordered by `at`
 * (newest-first), the entry inserted at its observation time rather than
 * blindly prepended → trim by count and age → atomic rewrite. A parse/shape
 * error on the existing file aborts **without** clobbering it (better to keep
 * the user's log than overwrite data we failed to understand).
 *
 * @return TRUE if a new entry was written; FALSE with @error unset when the
 *         observation merged into an existing entry (or changed nothing), or
 *         FALSE with @error set on failure.
 */
static gboolean
record_locked (MkHistory *self, JsonObject *snap, const char *instance,
               const char *name, const char *at, GDateTime *at_dt,
               GError **error)
{
  /* Read the existing newest-first array. A missing or empty file means no
   * entries; a present-but-unparseable/ill-shaped file aborts the write. */
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonArray *existing = NULL;
  if (g_file_test (self->path, G_FILE_TEST_EXISTS))
    {
      GError *perr = NULL;
      if (!json_parser_load_from_file (parser, self->path, &perr))
        {
          g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_PARSE,
                       "parse %s: %s", self->path, perr->message);
          g_clear_error (&perr);
          return FALSE;
        }
      JsonNode *root = json_parser_get_root (parser);
      if (root != NULL && JSON_NODE_HOLDS_ARRAY (root))
        existing = json_node_get_array (root);
      else if (root != NULL)
        {
          g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_INVALID,
                       "%s: top-level value is not a JSON array", self->path);
          return FALSE;
        }
      /* root == NULL: an empty file, treated as no entries. */
    }

  /* Dedup key: prefer file, fall back to the library id. */
  const char *file =
    json_object_get_string_member_with_default (snap, "file", NULL);
  if (file != NULL && *file == '\0')
    file = NULL;
  gboolean have_id = json_object_has_member (snap, "id");
  gint64 id = json_object_get_int_member_with_default (snap, "id", -1);
  if (file == NULL && !have_id)
    return FALSE; /* nothing identifies this item — can't dedup, don't log */

  /* Merge or append? */
  gboolean matched_newest = FALSE; /* coarsen `last_seen` only for this case */
  gint match = find_merge_match (existing, instance, file, have_id, id, at_dt,
                                 &matched_newest);
  g_autoptr (JsonNode) entry = NULL;
  g_autoptr (GDateTime) entry_at = NULL; /* sort key; NULL = keep position */
  if (match < 0)
    {
      entry = compose_entry (snap, instance, name, at);
      entry_at = g_date_time_ref (at_dt);
    }
  else
    {
      gboolean changed = FALSE;
      entry = merge_entry (json_array_get_element (existing, (guint) match),
                           at, at_dt, matched_newest, &entry_at, &changed);
      if (!changed)
        return FALSE; /* the sighting added nothing: leave the file alone */
    }

  /* Re-emit the array with the entry at its `at`-sorted position (an entry
   * with an unparseable stamp doesn't anchor the scan — the insertion point
   * is the first *datable* entry no newer), trimmed by count and age. */
  g_autoptr (GDateTime) cutoff =
    g_date_time_add_days (at_dt, -MK_HISTORY_MAX_AGE_DAYS);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  gboolean inserted = FALSE;
  guint kept = 0;
  if (existing != NULL)
    {
      guint n = json_array_get_length (existing);
      for (guint i = 0; i < n && kept < MK_HISTORY_MAX; i++)
        {
          JsonObject *e = json_array_get_object_element (existing, i);
          if (e == NULL)
            continue;
          if ((gint) i == match)
            {
              /* the merged original — superseded by @entry, which a merge
               * with an unparseable `at` re-emits right here, in place */
              if (!inserted && entry_at == NULL)
                {
                  json_builder_add_value (b, json_node_ref (entry));
                  inserted = TRUE;
                  kept++;
                }
              continue;
            }
          g_autoptr (GDateTime) edt = entry_time (e, "at");
          if (!inserted && entry_at != NULL
              && edt != NULL && g_date_time_compare (edt, entry_at) <= 0)
            {
              json_builder_add_value (b, json_node_ref (entry));
              inserted = TRUE;
              kept++;
              if (kept >= MK_HISTORY_MAX)
                break;
            }
          /* Age trim: drop anything older than the cutoff. A stamp we
           * can't parse is kept rather than silently dropped. */
          if (edt != NULL && g_date_time_compare (edt, cutoff) < 0)
            continue;
          json_builder_add_value (b,
                                  json_node_copy (json_array_get_element (existing, i)));
          kept++;
        }
    }
  if (!inserted) /* newest of all, older than everything kept, or empty log */
    json_builder_add_value (b, json_node_ref (entry));
  json_builder_end_array (b);
  g_autoptr (JsonNode) outroot = json_builder_get_root (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);
  json_generator_set_indent (gen, 2);
  json_generator_set_root (gen, outroot);
  g_autofree char *data = json_generator_to_data (gen, NULL);

  if (!atomic_write (self->path, data, error))
    return FALSE;
  return match < 0; /* TRUE only when a NEW entry was appended */
}

/**
 * mk_history_record:
 * @self: the history log.
 * @instance: the instance config key.
 * @name: the instance display label, or NULL.
 * @snapshot: the canonical player_state() now-playing object.
 *
 * The write path. See the header for the contract. Does the cheap
 * pre-lock checks ("is anything loaded") and stamps the observation time
 * **before** taking the lock — a writer delayed by contention still records
 * when it actually observed the play, and record_locked() inserts by that
 * stamp, so concurrent flock-serialized writers converge on the same
 * time-ordered log whatever order they land in. Then serialises the whole
 * read-modify-write under an exclusive flock on a sidecar lock file so
 * concurrent servers can't lose each other's appends. Best-effort:
 * every failure path warns to stderr and returns FALSE rather than propagating,
 * so a logging hiccup never fails the originating tool call.
 *
 * @return TRUE if a new entry was written; FALSE if it merged into an
 *         existing one, was skipped, or on error.
 */
gboolean
mk_history_record (MkHistory *self, const char *instance, const char *name,
                   JsonNode *snapshot)
{
  g_return_val_if_fail (self != NULL, FALSE);

  /* Nothing to record unless something is loaded. Cheap, pre-lock. */
  if (snapshot == NULL || !JSON_NODE_HOLDS_OBJECT (snapshot))
    return FALSE;
  JsonObject *snap = json_node_get_object (snapshot);
  if (g_strcmp0 (json_object_get_string_member_with_default (snap, "state",
                                                             "stopped"),
                 "stopped")
      == 0)
    return FALSE;

  /* The observation timestamp, fixed before any waiting on the lock. */
  g_autoptr (GDateTime) at_dt = g_date_time_new_now_utc ();
  g_autofree char *at = g_date_time_format_iso8601 (at_dt); /* UTC …Z */

  g_autofree char *dir = g_path_get_dirname (self->path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_warning ("mcp-kodi: history: create %s: %s", dir, g_strerror (errno));
      return FALSE;
    }

  /* Serialize the read-modify-write on a sidecar lock: a stable inode
   * the atomic rename() can't swap out from under us. */
  g_autofree char *lockpath = g_strconcat (self->path, ".lock", NULL);
  int lockfd = g_open (lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lockfd == -1)
    {
      g_warning ("mcp-kodi: history: open lock %s: %s", lockpath,
                 g_strerror (errno));
      return FALSE;
    }
  if (flock (lockfd, LOCK_EX) != 0)
    {
      g_warning ("mcp-kodi: history: lock %s: %s", lockpath, g_strerror (errno));
      close (lockfd);
      return FALSE;
    }

  GError *error = NULL;
  gboolean appended =
    record_locked (self, snap, instance, name, at, at_dt, &error);
  if (error != NULL)
    {
      g_warning ("mcp-kodi: history: %s", error->message);
      g_clear_error (&error);
    }

  flock (lockfd, LOCK_UN);
  close (lockfd);
  return appended;
}

/* ---- read path ----------------------------------------------------------- */

/**
 * in_window:
 * @e: a history entry object.
 * @since: lower bound (inclusive), or NULL for open.
 * @until: upper bound (inclusive), or NULL for open.
 *
 * Tests whether @e's `at` timestamp falls within [@since, @until]. An entry
 * whose `at` is missing or not parseable as ISO-8601 is treated as in-range —
 * kept, not silently dropped, mirroring the age-trim's rule: we never
 * hide data we merely failed to understand.
 *
 * @return TRUE if @e is within the window (or its stamp is unparseable).
 */
static gboolean
in_window (JsonObject *e, GDateTime *since, GDateTime *until)
{
  const char *at = json_object_get_string_member_with_default (e, "at", NULL);
  if (at == NULL)
    return TRUE;
  g_autoptr (GDateTime) dt = g_date_time_new_from_iso8601 (at, NULL);
  if (dt == NULL)
    return TRUE; /* keep what we can't parse */
  if (since != NULL && g_date_time_compare (dt, since) < 0)
    return FALSE;
  if (until != NULL && g_date_time_compare (dt, until) > 0)
    return FALSE;
  return TRUE;
}

/**
 * parse_bound:
 * @value: an ISO-8601 timestamp string, or NULL/empty for "no bound".
 * @label: the argument name, for the error message ("since"/"until").
 * @out: return location for the parsed GDateTime (set to NULL when no bound).
 * @error: return location for a GError.
 *
 * Parses an optional window bound. NULL or "" yields *@out = NULL (open); a
 * non-empty string that is not valid ISO-8601 is a hard error — better to tell
 * the caller their filter was malformed than to silently widen the window.
 *
 * @return TRUE on success (bound parsed or absent); FALSE with @error set.
 */
static gboolean
parse_bound (const char *value, const char *label, GDateTime **out,
             GError **error)
{
  *out = NULL;
  if (value == NULL || *value == '\0')
    return TRUE;
  *out = g_date_time_new_from_iso8601 (value, NULL);
  if (*out == NULL)
    {
      g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_INVALID,
                   "%s: not an ISO-8601 timestamp: %s", label, value);
      return FALSE;
    }
  return TRUE;
}

/**
 * load_locked:
 * @self: the history log.
 * @parser: out; receives a new JsonParser owning the loaded tree on success.
 * @entries: out; the top-level array, borrowed from @parser, or NULL when the
 *           file is missing/empty.
 * @error: return location for a GError.
 *
 * Reads and parses the log under a **shared** flock on the sidecar
 * lock — it excludes a writer's exclusive lock so we never parse a
 * half-rewritten file, but lets concurrent reads proceed. The lock is held only
 * across the load: json_parser_load_from_file() pulls the whole file into
 * memory, so once it returns the tree is ours and the lock can drop. A missing
 * file is success with *@entries = NULL (zero entries); a present but
 * unparseable or non-array file is an error.
 *
 * @return TRUE on success; FALSE with @error set on lock/parse/shape failure.
 */
static gboolean
load_locked (MkHistory *self, JsonParser **parser, JsonArray **entries,
             GError **error)
{
  *parser = NULL;
  *entries = NULL;
  if (!g_file_test (self->path, G_FILE_TEST_EXISTS))
    return TRUE; /* no file yet → no entries */

  g_autofree char *lockpath = g_strconcat (self->path, ".lock", NULL);
  int lockfd = g_open (lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lockfd == -1)
    {
      g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_IO,
                   "open lock %s: %s", lockpath, g_strerror (errno));
      return FALSE;
    }
  if (flock (lockfd, LOCK_SH) != 0)
    {
      g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_IO, "lock %s: %s",
                   lockpath, g_strerror (errno));
      close (lockfd);
      return FALSE;
    }

  JsonParser *p = json_parser_new ();
  GError *perr = NULL;
  gboolean ok = json_parser_load_from_file (p, self->path, &perr);
  flock (lockfd, LOCK_UN);
  close (lockfd);

  if (!ok)
    {
      g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_PARSE,
                   "parse %s: %s", self->path, perr->message);
      g_clear_error (&perr);
      g_object_unref (p);
      return FALSE;
    }

  JsonNode *root = json_parser_get_root (p);
  if (root != NULL && JSON_NODE_HOLDS_ARRAY (root))
    *entries = json_node_get_array (root);
  else if (root != NULL)
    {
      g_set_error (error, MK_HISTORY_ERROR, MK_HISTORY_ERROR_INVALID,
                   "%s: top-level value is not a JSON array", self->path);
      g_object_unref (p);
      return FALSE;
    }
  /* root == NULL: an empty file, treated as no entries. */
  *parser = p;
  return TRUE;
}

/**
 * mk_history_read:
 * @self: the history log.
 * @instance: restrict to this instance key, or NULL for all instances.
 * @since: ISO-8601 lower bound (inclusive), or NULL/"" for open.
 * @until: ISO-8601 upper bound (inclusive), or NULL/"" for open.
 * @limit: maximum entries to return; ≤ 0 means no cap.
 * @total: out; set to the number of matches before the limit, or NULL.
 * @error: return location for a GError.
 *
 * The read path. See the header for the contract. Parses the bounds, loads
 * the log under a shared lock, then walks it newest-first keeping
 * entries whose `instance` matches and whose `at` lies in [@since, @until],
 * collecting at most @limit of them — so the result is the newest matches. The
 * walk keeps counting matches past the cap to report @total, from which the
 * caller derives `truncated`.
 *
 * @return a newly allocated JSON array node (newest-first), or NULL with @error
 *         set; free with json_node_unref().
 */
JsonNode *
mk_history_read (MkHistory *self, const char *instance, const char *since,
                 const char *until, gint64 limit, gint64 *total, GError **error)
{
  g_return_val_if_fail (self != NULL, NULL);
  if (total != NULL)
    *total = 0;

  g_autoptr (GDateTime) since_dt = NULL;
  g_autoptr (GDateTime) until_dt = NULL;
  if (!parse_bound (since, "since", &since_dt, error)
      || !parse_bound (until, "until", &until_dt, error))
    return NULL;

  g_autoptr (JsonParser) parser = NULL;
  JsonArray *entries = NULL;
  if (!load_locked (self, &parser, &entries, error))
    return NULL;

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  gint64 matched = 0, kept = 0;
  if (entries != NULL)
    {
      guint n = json_array_get_length (entries);
      for (guint i = 0; i < n; i++)
        {
          JsonObject *e = json_array_get_object_element (entries, i);
          if (e == NULL)
            continue;
          if (instance != NULL
              && g_strcmp0 (json_object_get_string_member_with_default (
                              e, "instance", NULL),
                            instance)
                   != 0)
            continue;
          if (!in_window (e, since_dt, until_dt))
            continue;
          matched++;
          /* Past the cap we keep counting (for @total) but stop collecting. The
           * file is newest-first, so the kept ones are the most recent. */
          if (limit > 0 && kept >= limit)
            continue;
          json_builder_add_value (
            b, json_node_copy (json_array_get_element (entries, i)));
          kept++;
        }
    }
  json_builder_end_array (b);
  if (total != NULL)
    *total = matched;
  return json_builder_get_root (b);
}
