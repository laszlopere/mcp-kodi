/* mcp-kodi — playback history log (skeleton). See mk-history.h and ../TODO.md §13.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 */

#include "config.h"

#include "mk-history.h"

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
