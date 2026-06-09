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
 * Capture is **call-driven only** (§13.2): the module never polls Kodi and never
 * subscribes to its WebSocket push (§2.1, §10.1). The only moments we learn what
 * is playing are the moments a tool call runs and produces a now-playing
 * snapshot — so the log records *what the assistant caused or observed*, not a
 * complete audit of the box. Known, accepted blind spots: playback started from
 * the physical remote / Kodi UI is invisible unless a later call happens to
 * snapshot it (§13.2.1), and we capture only the item playing at the moment of
 * the call — the first item when a play starts — not the tracks Kodi advances to
 * on its own (no playlist/queue tracking yet, §13.2.2). Honest about its gaps;
 * good enough for "what did we play" (§13.2.3).
 *
 * The raw material is free: every playback-affecting tool already ends by
 * building the canonical now-playing snapshot (player_state(), §5.4 — state,
 * type, file, label, title, time, totaltime). History **reuses that exact
 * snapshot** (§13.3); a record is composed from it plus the instance key and a
 * capture timestamp, with no extra Kodi round-trip. The lone exception is the
 * `rpc` escape hatch (§11.6.6), which returns Kodi's raw result and takes no
 * snapshot — so after a successful `rpc` it issues one extra snapshot purely to
 * feed history, as a side effect that must not change `rpc`'s verbatim return
 * (§13.3.1). That wiring lands with the write path (§13.5–§13.9).
 *
 * Entry fields (§13.4 — the per-item fields are now all settled; only how the
 * wider GetItem is sourced (§13.4.3) and any deeper metadata (§13.4.4) remain
 * open):
 *   - `at`    — the capture timestamp (§13.4.0): server wall-clock at the moment
 *               the snapshot is taken and the entry written (≈ when playback
 *               started, §13.2.2), from g_date_time_*. Stored as ISO-8601 **with
 *               an explicit timezone** — UTC `…Z` is the default; never a bare
 *               local time, which makes range queries wrong across DST/zone
 *               changes. It is the sort key, the filter for "last week /
 *               yesterday / last month" queries (§13.10), and what drives the
 *               age-based trim (§13.8.2). Records the *start*, not completion or
 *               duration (§13.2).
 *   - `kind`  — the player kind (§13.4.1): `audio` or `video`, the snapshot's
 *               existing `type` from Player.GetActivePlayers. Tells music from
 *               video but not movie from episode from music video.
 *   - `media` — the real media type (§13.4.1): `song`/`episode`/`movie`/
 *               `musicvideo`/`picture`/`channel`/`unknown`. Kodi injects it into
 *               the Player.GetItem reply as an **identity field** (not a
 *               requestable property), so it is free; player_state() surfaces it
 *               under `media` — a key other than `type` (the snapshot already
 *               spends `type` on the player kind), hence the `kind` + `media`
 *               split.
 *   - `id`    — the library id (§13.4.1): the auto-injected songid/episodeid/
 *               movieid, also free. A best-effort replay handle for
 *               Player.Open {<media>id: N}, NOT a durable key — it is
 *               library-scoped and not stable across a library clean/rescan
 *               (§13.4.1.1), so treat file/label/showtitle/album as the durable
 *               identifiers. A `playfile` of a path not in the library can't be
 *               enriched → `media` is "unknown" and `id` is -1.
 *   - show/episode (§13.4.2): for a TV episode, `showtitle` (string) plus
 *               `season` and `episode` (integers) — which show, which episode.
 *   - album/song (§13.4.2): for music, `album` (string), `artist` (array of
 *               strings, e.g. ["Abba"]) and `track` (integer).
 * Unlike `media`/`id` (auto-injected), the §13.4.2 fields must be named in the
 * `properties` array of Player.GetItem — but they are all valid List.Item.Base
 * properties reachable in the **same single call** (which today asks only
 * ["title","file"]), so widening that list costs no extra Kodi round-trip, only
 * a longer field list. Per media type an entry carries only the fields that
 * apply (a movie has neither show/episode nor album/artist); omit what's empty.
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
