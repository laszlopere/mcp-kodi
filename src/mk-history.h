/* mcp-kodi — playback history log (skeleton).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
 *
 * A chronological, cross-instance log of what was actually played. Unlike the
 * per-instance state — forward-looking intent, one file per box — this is a
 * single backward-looking, append-mostly log spanning *all* instances, in its
 * own file with its own lifecycle. History and state
 * deliberately do **not** share storage, so a history append can never race a
 * state write.
 *
 * Capture has two feeders, both arriving as player_state() snapshots — this
 * module itself never talks to Kodi, and never subscribes to its WebSocket
 * push. (1) **Call-driven**: every playback-affecting tool call ends in a
 * snapshot, so the log records what the assistant caused or observed. (2)
 * **Live monitoring** (mk_tools_poll_history, on a ~2-minute timer in main):
 * every configured box is snapshotted each round, so playback started outside
 * the server — the TV remote, the Kodi UI — and the tracks Kodi advances to
 * on its own are captured too, within a round of starting. The remaining,
 * accepted blind spot is deliberate: nothing is observed while no mcp-kodi
 * process runs, so this is session-lifetime coverage ("what played while the
 * assistant was up"), not a daemon's complete audit of the box. Honest about
 * its gaps; good enough for "what did we play".
 *
 * The raw material is free: every playback-affecting tool already ends by
 * building the canonical now-playing snapshot (player_state() — state, type,
 * file, label, title, time, totaltime). History **reuses that exact
 * snapshot**; a record is composed from it plus the instance key and a
 * capture timestamp, with no extra Kodi round-trip. The lone exception is the
 * `rpc` escape hatch, which returns Kodi's raw result and takes no snapshot —
 * so after a successful `rpc` it issues one extra snapshot purely to feed
 * history, as a side effect that must not change `rpc`'s verbatim return. That
 * wiring lands with the write path.
 *
 * Entry fields (the per-item fields are now all settled; only how the wider
 * GetItem is sourced and any deeper metadata remain open):
 *   - `at`    — the capture timestamp: server wall-clock at the moment
 *               the snapshot is taken and the entry written (≈ when playback
 *               started), from g_date_time_*. Stored as ISO-8601 **with
 *               an explicit timezone** — UTC `…Z` is the default; never a bare
 *               local time, which makes range queries wrong across DST/zone
 *               changes. It is the sort key, the filter for "last week /
 *               yesterday / last month" queries, and what drives
 *               the age-based trim. Records the *start*, not completion or
 *               duration. When several writers observe the same play, `at`
 *               converges on the **earliest** sighting.
 *   - `last_seen` — the latest sighting of the same play, same ISO-8601
 *               form; written only once a play is re-observed (a repeat
 *               snapshot, or another process's sighting merging in), so its
 *               absence means "seen once". `at` stays the sort key. For the
 *               currently-playing item the per-poll advance is **coarsened**
 *               (MK_HISTORY_LAST_SEEN_STEP): rather than rewrite the whole log
 *               every ~2-minute poll to nudge the stamp, the bump is persisted
 *               only once it clears the step, so `last_seen` trails the true
 *               latest sighting by at most that step.
 *   - `kind`  — the player kind: `audio` or `video`, the snapshot's
 *               existing `type` from Player.GetActivePlayers. Tells music from
 *               video but not movie from episode from music video.
 *   - `media` — the real media type: `song`/`episode`/`movie`/
 *               `musicvideo`/`picture`/`channel`/`unknown`. Kodi injects it into
 *               the Player.GetItem reply as an **identity field** (not a
 *               requestable property), so it is free; player_state() surfaces it
 *               under `media` — a key other than `type` (the snapshot already
 *               spends `type` on the player kind), hence the `kind` + `media`
 *               split.
 *   - `id`    — the library id: the auto-injected songid/episodeid/
 *               movieid, also free. A best-effort replay handle for
 *               Player.Open {<media>id: N}, NOT a durable key — it is
 *               library-scoped and not stable across a library clean/rescan,
 *               so treat file/label/showtitle/album as the durable
 *               identifiers. A `playfile` of a path not in the library can't be
 *               enriched → `media` is "unknown" and `id` is -1.
 *   - show/episode: for a TV episode, `showtitle` (string) plus
 *               `season` and `episode` (integers) — which show, which episode.
 *   - album/song: for music, `album` (string), `artist` (array of
 *               strings, e.g. ["Abba"]) and `track` (integer).
 * Unlike `media`/`id` (auto-injected), per-media fields must be named in the
 * `properties` array of Player.GetItem — but they are all valid List.Item.Base
 * properties reachable in the **same single call** (which today asks only
 * ["title","file"]), so widening that list costs no extra Kodi round-trip, only
 * a longer field list. Per media type an entry carries only the fields that
 * apply (a movie has neither show/episode nor album/artist); omit what's empty.
 *
 * The module owns its storage path
 * (${XDG_STATE_HOME:-~/.local/state}/mcp-kodi/history.json) and
 * lifecycle. The write path — record/dedup/lock/trim — and the read path
 * (mk_history_read) both land here; the `history` tool that surfaces the read
 * path to clients lives in mk-tools.c.
 */

#ifndef MK_HISTORY_H
#define MK_HISTORY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* The history log: the resolved path to the one global log file. Entries are
 * not held in memory — each write is a locked read-modify-write of the file,
 * added when the write path lands. */
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

/* Record what is playing in @snapshot to the log. @snapshot is the
 * canonical player_state() now-playing object; @instance is its
 * config key and @name the human display label (@name may be NULL). Runs the
 * full locked read-modify-write: nothing is written unless something is
 * loaded. The observation is matched against @instance's recent entries —
 * its newest entry on item identity alone, older ones only within a short
 * time window of their [at, last_seen] coverage. A match **merges**: `at`
 * keeps the earliest sighting, `last_seen` the latest, and the file is
 * rewritten only when that changed something — so recording is idempotent
 * under concurrent flock-serialized writers, and a late or out-of-order
 * sighting of an already-logged play never spawns a duplicate. No match
 * composes a new entry, inserted by its `at` (the array stays newest-first
 * by observation time, not arrival). Either way the array is trimmed by
 * count and age and atomically rewritten under an exclusive lock.
 *
 * Best-effort: a logging failure must never fail the originating tool
 * call, so this reports no GError — it warns to stderr and returns. The
 * boolean says only whether a *new* entry was appended (FALSE = nothing
 * loaded, the observation merged into an existing entry, or a swallowed write
 * error); callers ignore it, but it lets tests assert the merge/skip
 * behaviour.
 *
 * @return TRUE if a new entry was written; FALSE if merged/skipped or on
 *         error. */
gboolean mk_history_record (MkHistory  *self,
                            const char *instance,
                            const char *name,
                            JsonNode   *snapshot);

/* Read entries back from the log — the mirror of
 * mk_history_record. @instance NULL returns every box's entries (the log is
 * cross-instance); a key restricts to that box. @since/@until are ISO-8601
 * bounds (NULL = open on that end); an `at` we can't parse is kept, not
 * dropped (as the age-trim). @limit ≤ 0 means no cap; otherwise at most
 * @limit *newest* matches are returned (the file is newest-first). @total,
 * when non-NULL, is set to the number of entries matching @instance+window
 * before the limit, so the caller can tell it truncated.
 *
 * Unlike the best-effort write path this reports failures: a malformed
 * @since/@until, an unparseable or ill-shaped file, or an I/O error returns NULL
 * with @error set. A missing file is not an error — it reads as zero entries.
 * Runs under a shared flock on the sidecar lock, so it never parses a
 * file mid-rewrite.
 *
 * @return a newly allocated JSON array node of entries (newest-first), or NULL
 *         with @error set; free with json_node_unref(). */
JsonNode *mk_history_read (MkHistory  *self,
                           const char *instance,
                           const char *since,
                           const char *until,
                           gint64      limit,
                           gint64     *total,
                           GError    **error);

G_END_DECLS

#endif /* MK_HISTORY_H */
