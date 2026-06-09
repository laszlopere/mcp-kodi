/* mcp-kodi — playback history log (skeleton). See mk-history.h and ../TODO.md §13.
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

/* Retention bounds (§13.8), tuned once we see real volume. With sparse,
 * call-driven entries (§13.2) 10k rows is many months — comfortably past
 * "last month". */
#define MK_HISTORY_MAX          10000 /* keep newest this many entries (§13.8.1) */
#define MK_HISTORY_MAX_AGE_DAYS 180   /* drop entries older than this (§13.8.2)  */

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
 * ${XDG_STATE_HOME:-~/.local/state}/mcp-kodi/history.json (§13.6), computed once
 * and cached. This is state, not config — a backward-looking log of what played
 * — so it lives under XDG_STATE_HOME, separate from the §7 config and the §8
 * per-instance state (§13.1).
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
 * directory and file are created lazily by the first write (§13.6), once the
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

/* ---- write path (§13.9.1) ------------------------------------------------ */

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
 * (§13.4) needs no further filtering here.
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
 * (the player kind, §13.4.1) under the entry's `kind`.
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
 * @snap: the now-playing snapshot (player_state(), §5.4).
 * @instance: the instance config key.
 * @name: the instance display label, or NULL.
 * @at: the capture timestamp, ISO-8601 with timezone (§13.4.0).
 *
 * Builds one history entry (§13.4) from the snapshot plus the instance
 * key/name and capture time: `at`/`instance`/`name`, the player kind as `kind`
 * (from the snapshot's `type`), then `media`/`id` (§13.4.1) and the per-media
 * identifiers (§13.4.2) copied straight across — present only where the
 * snapshot carries them. `state`/`time`/`totaltime` are deliberately not stored
 * (§13.4): the entry asserts only "this was played at `at`".
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
  hist_copy_as (b, snap, "type", "kind"); /* player kind (§13.4.1) */
  hist_copy (b, snap, "media");           /* real media type (§13.4.1) */
  hist_copy (b, snap, "id");              /* library id (§13.4.1) */
  hist_copy (b, snap, "title");
  hist_copy (b, snap, "showtitle"); /* §13.4.2 */
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
 * is_duplicate:
 * @existing: the current entries, newest-first, or NULL.
 * @instance: the instance whose most recent entry we compare against.
 * @file: the snapshot's file path, or NULL.
 * @have_id: whether the snapshot carries a library id.
 * @id: that library id (valid only when @have_id).
 *
 * Implements the §13.5.2 "new item" test: find @instance's most recent entry
 * (the first one for it in the newest-first array) and report whether it is the
 * same thing just re-observed — same `file`, or same `id` when neither carries
 * a file. Only the most recent entry per instance matters; an older repeat is
 * still a distinct play.
 *
 * @return TRUE if the snapshot duplicates that entry (caller should skip).
 */
static gboolean
is_duplicate (JsonArray *existing, const char *instance, const char *file,
              gboolean have_id, gint64 id)
{
  if (existing == NULL)
    return FALSE;
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
      /* most recent entry for this instance */
      const char *efile =
        json_object_get_string_member_with_default (e, "file", NULL);
      if (file != NULL && efile != NULL)
        return g_strcmp0 (efile, file) == 0;
      return have_id && json_object_has_member (e, "id")
             && json_object_get_int_member_with_default (e, "id", -2) == id;
    }
  return FALSE; /* nothing logged for this instance yet */
}

/**
 * atomic_write:
 * @path: the target file.
 * @data: the bytes to write.
 * @error: return location for a GError.
 *
 * Writes @data to a 0600 temp file in @path's directory, fsync()s it, then
 * rename()s it over @path — the §7.4/§8.4 atomic-replacement discipline
 * (§13.7.2), run inside the caller's lock.
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
 * @snap: the now-playing snapshot, already known to be loaded (§13.5.1).
 * @instance: the instance config key.
 * @name: the instance display label, or NULL.
 * @error: return location for a GError.
 *
 * The critical section of mk_history_record(), run under the exclusive lock
 * (§13.7.2): read the current array → dedup (§13.5.2) → compose/prepend the new
 * entry → trim by count and age (§13.8) → atomic rewrite. A parse/shape error
 * on the existing file aborts **without** clobbering it (better to keep the
 * user's log than overwrite data we failed to understand).
 *
 * @return TRUE if an entry was written; FALSE with @error unset when the play
 *         is a duplicate to skip, or FALSE with @error set on failure.
 */
static gboolean
record_locked (MkHistory *self, JsonObject *snap, const char *instance,
               const char *name, GError **error)
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

  /* §13.5.2 dedup key: prefer file, fall back to the library id. */
  const char *file =
    json_object_get_string_member_with_default (snap, "file", NULL);
  if (file != NULL && *file == '\0')
    file = NULL;
  gboolean have_id = json_object_has_member (snap, "id");
  gint64 id = json_object_get_int_member_with_default (snap, "id", -1);
  if (file == NULL && !have_id)
    return FALSE; /* nothing identifies this item — can't dedup, don't log */

  if (is_duplicate (existing, instance, file, have_id, id))
    return FALSE; /* same thing re-observed (§13.5.2): skip, no error */

  /* Compose the new entry and the new newest-first array, trimmed (§13.8). */
  g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
  g_autoptr (GDateTime) cutoff =
    g_date_time_add_days (now, -MK_HISTORY_MAX_AGE_DAYS);
  g_autofree char *at = g_date_time_format_iso8601 (now); /* …Z, §13.4.0 */
  g_autoptr (JsonNode) entry = compose_entry (snap, instance, name, at);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  json_builder_add_value (b, json_node_ref (entry)); /* newest first (§13.6) */
  guint kept = 1;
  if (existing != NULL)
    {
      guint n = json_array_get_length (existing);
      for (guint i = 0; i < n && kept < MK_HISTORY_MAX; i++)
        {
          JsonObject *e = json_array_get_object_element (existing, i);
          if (e == NULL)
            continue;
          /* Age trim (§13.8.2): drop anything older than the cutoff. A stamp we
           * can't parse is kept rather than silently dropped. */
          const char *eat =
            json_object_get_string_member_with_default (e, "at", NULL);
          if (eat != NULL)
            {
              g_autoptr (GDateTime) edt = g_date_time_new_from_iso8601 (eat, NULL);
              if (edt != NULL && g_date_time_compare (edt, cutoff) < 0)
                continue;
            }
          json_builder_add_value (b,
                                  json_node_copy (json_array_get_element (existing, i)));
          kept++;
        }
    }
  json_builder_end_array (b);
  g_autoptr (JsonNode) outroot = json_builder_get_root (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);
  json_generator_set_indent (gen, 2);
  json_generator_set_root (gen, outroot);
  g_autofree char *data = json_generator_to_data (gen, NULL);

  return atomic_write (self->path, data, error);
}

/**
 * mk_history_record:
 * @self: the history log.
 * @instance: the instance config key.
 * @name: the instance display label, or NULL.
 * @snapshot: the canonical player_state() now-playing object (§5.4/§13.3).
 *
 * The write path (§13.9.1). See the header for the contract. Does the cheap
 * pre-lock checks (§13.5.1 "is anything loaded"), then serialises the whole
 * read-modify-write under an exclusive flock on a sidecar lock file (§13.7) so
 * concurrent servers can't lose each other's appends. Best-effort (§13.9.2):
 * every failure path warns to stderr and returns FALSE rather than propagating,
 * so a logging hiccup never fails the originating tool call.
 *
 * @return TRUE if a new entry was written; FALSE if skipped or on error.
 */
gboolean
mk_history_record (MkHistory *self, const char *instance, const char *name,
                   JsonNode *snapshot)
{
  g_return_val_if_fail (self != NULL, FALSE);

  /* §13.5.1: nothing to record unless something is loaded. Cheap, pre-lock. */
  if (snapshot == NULL || !JSON_NODE_HOLDS_OBJECT (snapshot))
    return FALSE;
  JsonObject *snap = json_node_get_object (snapshot);
  if (g_strcmp0 (json_object_get_string_member_with_default (snap, "state",
                                                             "stopped"),
                 "stopped")
      == 0)
    return FALSE;

  g_autofree char *dir = g_path_get_dirname (self->path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_warning ("mcp-kodi: history: create %s: %s", dir, g_strerror (errno));
      return FALSE;
    }

  /* Serialize the read-modify-write on a sidecar lock (§13.7.1): a stable inode
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
  gboolean appended = record_locked (self, snap, instance, name, &error);
  if (error != NULL)
    {
      g_warning ("mcp-kodi: history: %s", error->message);
      g_clear_error (&error);
    }

  flock (lockfd, LOCK_UN);
  close (lockfd);
  return appended;
}
