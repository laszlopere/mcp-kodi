/* mcp-kodi — test stub for the playback-history log (mk-history).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * Drop-in replacement for src/mk-history.c that performs NO file I/O: it never
 * opens, locks, reads or writes history.json. mk_history_record() just counts
 * the calls it received (so a test can assert the write path was reached) and
 * mk_history_read() returns an empty array. This keeps the tools tests free of
 * any on-disk side effect.
 *
 * Implements the full mk-history.h surface so it satisfies the linker in place
 * of the real module.
 */

#include "mk-history.h"

/* Number of mk_history_record() calls since the last reset; inspected by tests. */
int stub_history_record_calls = 0;

void
stub_history_reset (void)
{
  stub_history_record_calls = 0;
}

struct _MkHistory
{
  char *path;
};

G_DEFINE_QUARK (mk-history-error-quark, mk_history_error)

const char *
mk_history_default_path (void)
{
  return "/dev/null/stub-history.json"; /* never touched */
}

MkHistory *
mk_history_new (const char *path)
{
  MkHistory *self = g_new0 (MkHistory, 1);
  self->path = g_strdup (path != NULL ? path : mk_history_default_path ());
  return self;
}

void
mk_history_free (MkHistory *self)
{
  if (self == NULL)
    return;
  g_free (self->path);
  g_free (self);
}

const char *
mk_history_path (MkHistory *self)
{
  return self != NULL ? self->path : NULL;
}

gboolean
mk_history_record (MkHistory  *self,
                   const char *instance,
                   const char *name,
                   JsonNode   *snapshot)
{
  (void) self;
  (void) instance;
  (void) name;
  (void) snapshot;
  stub_history_record_calls++;
  return FALSE; /* "nothing appended" — no disk, best-effort like the real one */
}

JsonNode *
mk_history_read (MkHistory  *self,
                 const char *instance,
                 const char *since,
                 const char *until,
                 gint64      limit,
                 gint64     *total,
                 GError    **error)
{
  (void) self;
  (void) instance;
  (void) since;
  (void) until;
  (void) limit;
  (void) error;
  if (total != NULL)
    *total = 0;
  return json_node_init_array (json_node_alloc (), json_array_new ());
}
