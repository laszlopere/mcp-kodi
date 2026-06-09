/* mcp-kodi — playback history log (skeleton).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * A chronological, cross-instance log of what was actually played (§13). Unlike
 * the per-instance state (§8) — forward-looking intent, one file per box — this
 * is a single backward-looking, append-mostly log spanning *all* instances, in
 * its own file with its own lifecycle. History and state deliberately do **not**
 * share storage, so a history append can never race a state write (§13.1).
 *
 * This is the §13.1 foundation only: the module, its own storage path
 * (${XDG_STATE_HOME:-~/.local/state}/mcp-kodi/history.json, §13.6), and
 * lifecycle. The write path — record/dedup/lock/trim (§13.5–§13.9) — and the
 * read tool (§13.10) land later.
 */

#ifndef MK_HISTORY_H
#define MK_HISTORY_H

#include <glib.h>

G_BEGIN_DECLS

/* The history log: the resolved path to the one global log file. Entries are
 * not held in memory — each write is a locked read-modify-write of the file
 * (§13.7), added when the write path lands. */
typedef struct _MkHistory MkHistory;

#define MK_HISTORY_ERROR (mk_history_error_quark ())
GQuark mk_history_error_quark (void);

typedef enum
{
  MK_HISTORY_ERROR_PARSE,   /* file exists but is not valid JSON */
  MK_HISTORY_ERROR_INVALID, /* JSON is valid but not the expected array shape */
  MK_HISTORY_ERROR_IO,      /* open/lock/read/write/rename failure */
} MkHistoryError;

/* Path of the default history file (cached, owned by the library). */
const char *mk_history_default_path (void);

/* Create a history log backed by PATH (NULL → mk_history_default_path()). Does
 * not touch the filesystem — the file is created lazily on first write. */
MkHistory *mk_history_new  (const char *path);
void       mk_history_free (MkHistory *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MkHistory, mk_history_free)

/* The log file this instance writes to (the resolved PATH); owned by @self. */
const char *mk_history_path (MkHistory *self);

G_END_DECLS

#endif /* MK_HISTORY_H */
