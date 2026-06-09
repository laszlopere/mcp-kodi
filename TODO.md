# mcp-kodi — design & TODO

An MCP server that lets an AI assistant control a Kodi media player over Kodi's
JSON-RPC API. Written in C on the GLib stack. **No user-facing CLI** — the user
talks to the AI, the AI calls this MCP server, and the server speaks JSON-RPC to
Kodi.

This file is the spec. It is written from scratch and the implementation will be
**clean-room**: an independent project under the author's copyright, not derived
from or copied out of any other codebase. The notes in
[§1](#1-approach-borrowed-conceptually-not-by-code) describe an *approach* that
has been proven elsewhere; they are requirements/architecture, not source.

Every point is numbered `N.M` so any part can be referenced precisely (e.g. "see
4.3"). Checkboxes mark items the implementation must satisfy.

---

## 1. Approach (borrowed conceptually, not by code)

A separate GLib project of the author's already controls this same Kodi box. The
*techniques* worth reusing — restated here as our own requirements, with no code
carried over:

  [x] 1.1 **Stack:** GLib + GObject + GIO, built with **autotools**
  (`configure.ac` + `PKG_CHECK_MODULES`).
  [x] 1.2 **JSON:** `json-glib-1.0` for all parsing and serialisation
  (`JsonParser`, `JsonBuilder`, `JsonNode`, `JsonObject`, `JsonGenerator`).
  [x] 1.3 **HTTP:** `libsoup-3.0` as the HTTP client (`SoupSession`,
  `SoupMessage`, `soup_session_send_and_read`).
  [x] 1.4 **Transport to Kodi:** JSON-RPC POSTed over **HTTPS** to a Caddy
  reverse proxy in front of Kodi. The proxy uses `tls internal` (a self-signed
  cert), so the client must **accept the untrusted certificate** explicitly
  (equivalent to curl `-k`).
  [x] 1.5 **Config from a file + environment, never hardcoded:** read the Kodi
  host, HTTP Basic credentials, scheme, and a self-signed-cert flag from
  configuration rather than baking them in. This project uses its own JSON file
  (not the borrowed CLI's shell config) — see [§7](#7-configuration-file) for the
  format and location.

Everything below this section is original design for *this* project.

---

## 2. Scope & principles

  [ ] 2.1 **Stateless logic.** No long-lived in-memory session state beyond the
  live stdio loop. Each `tools/call` is self-contained: read config, talk to
  Kodi, return JSON. Any state that must persist between calls lives in a file
  (see [§7](#7-playback-state-file)).
  [ ] 2.2 **The server emits JSON, not prose for humans.** Tool results are
  compact, machine-readable text (JSON) the AI can reason over.
  [ ] 2.3 **Mirror the proven operation set** (ping, info, notify, transport
  controls, play-by-id, library search, random, episode listing, status,
  volume, mute) plus a generic `rpc` escape hatch for anything not modelled
  (opt-in per instance, off by default — §7.7, §11.6.6).
  [ ] 2.4 **No CLI.** The only interface is MCP over stdio.

---

## 3. MCP protocol (stdio transport)

  [ ] 3.1 **Framing:** newline-delimited JSON-RPC 2.0 messages on
  **stdin/stdout**, one JSON object per line, no embedded newlines. Logging goes
  to **stderr only** (stdout must stay pure protocol).
  [ ] 3.2 **Event loop:** a `GMainLoop` with a GIO async read on stdin
  (`GUnixInputStream` + `GDataInputStream`, read-line-async). EOF on stdin →
  quit the loop and exit cleanly.
  [ ] 3.3 **Lifecycle methods to implement:**
    [ ] 3.3.1 `initialize` → respond with `protocolVersion` (echo the client's
    requested version when we recognise it, else our latest known),
    `capabilities` (`{ "tools": {} }`), and `serverInfo` (`{ name, version }`).
    [ ] 3.3.2 `notifications/initialized` → no response (it's a notification).
    [ ] 3.3.3 `ping` → empty result `{}`.
    [ ] 3.3.4 `tools/list` → `{ "tools": [ ... ] }` (see
    [§5](#5-tool-surface)).
    [ ] 3.3.5 `tools/call` → `{ "content": [ { "type": "text",
    "text": "<json>" } ], "isError": <bool> }`.
  [ ] 3.4 **Errors:** JSON-RPC error objects with proper codes
  (`-32700` parse, `-32600` invalid request, `-32601` method not found,
  `-32602` invalid params, `-32603` internal). Tool-level failures (Kodi
  unreachable, RPC error) are returned as a normal `tools/call` result with
  `isError: true` and the error detail in the text content — not as a
  protocol-level error.
  [ ] 3.5 **Requests vs notifications:** a message with no `id` is a
  notification → never send a response.

---

## 4. Kodi JSON-RPC client

  [x] 4.1 Build requests with `JsonBuilder`:
  `{ "jsonrpc": "2.0", "id": <n>, "method": <m>, "params": <obj> }`.
  [x] 4.2 Calls are made *against a named instance* (see [§7](#7-configuration-file)):
  resolve the instance to its `scheme`/`host`/`auth`/`insecure`, then POST to
  `<scheme>://<host>/jsonrpc` with `Content-Type: application/json` and HTTP Basic
  auth. A tool that omits the instance uses the configured `default`.
  [x] 4.3 Reuse one `SoupSession` for the whole process across all instances
  (it can talk to several hosts). For each instance whose config sets `insecure`,
  accept the self-signed cert — connect the session's `accept-certificate` signal
  and accept only for that instance's host, not blanket.
  [x] 4.4 Synchronous send per tool call (`soup_session_send_and_read`) is fine —
  calls are serialised by the AI and latency is human-scale.
  [x] 4.5 Parse the response; surface Kodi's `error` member as a tool error.
  Return the `result` member (or a small shaped summary) as the tool's JSON text.

  [ ] 4.6 **Server-side setup:** how to enable Kodi's JSON-RPC web server and
  front it with the Caddy HTTPS reverse proxy is documented for end users in
  [docs/kodi-server-setup.md](docs/kodi-server-setup.md).

---

## 5. Tool surface

  [ ] 5.1 **Instance targeting:** every tool that talks to a player or device
  takes an optional `instance` arg (a configured instance name, e.g. `hall`,
  `bedroom`, `kids`). Omitted → the config `default`. The schema documents the
  available names. `search`/`episodes` are library lookups and may ignore it (any
  instance shares the same library), but accept it for uniformity.
  [ ] 5.2 Each tool has a JSON Schema `inputSchema`. Names and arguments:

  | Tool        | Args                                   | Kodi method(s) |
  |-------------|----------------------------------------|----------------|
  | `instances` | `action` (get/set/remove), `key?`, `name?`, `host?`, `auth?`, `scheme?`, `insecure?`, `default?` | none — read/write the server's own instance config (§7, §11.6.3); `get` masks credentials |
  | `ping`      | `instance?`                            | `JSONRPC.Ping` |
  | `info`      | `instance?`                            | `Application.GetProperties` (name, version, volume, muted) |
  | `notify`    | `instance?`, `title`, `message`, `displaytime?` | `GUI.ShowNotification` |
  | `players`   | `instance?`                            | `Player.GetActivePlayers` |
  | `playpause` | `instance?`                            | `Player.PlayPause` (on active player) |
  | `stop`      | `instance?`                            | `Player.Stop` (on active player) |
  | `play`      | `instance?`, `type` (song/album/artist/movie/episode/musicvideo), `id` | `Player.Open` with the matching `item` key |
  | `seek`      | `instance?`, `time` (h/m/s) **or** `percentage` | `Player.Seek` on the active player |
  | `handoff`   | `from`, `to`                           | capture active item + position on `from` → `Player.Stop` on `from` → `Player.Open` on `to` → `Player.Seek` to that position |
  | `search`    | `type` (music/tv-show/movie), `artist?`, `title?`, `season?`, `number?`, `limit?`, `offset?`, `count?` | resolves the per-type chain to **leaf files**; reports `total`; pages via `limit`/`offset` — see §5.10 |
  | `random`    | `instance?`, `type` (episode/movie/song), `query?`  | resolve filter → pick → `Player.Open` |
  | `episodes`  | `show`, `filter?` (SxxExx or text)     | `VideoLibrary.GetEpisodes` |
  | `status`    | `instance?` (`"*"` = all)              | `Player.GetActivePlayers` + `Player.GetItem` + `Player.GetProperties`; per instance |
  | `volume`    | `instance?`, `level` (0–100)           | `Application.SetVolume` |
  | `mute`      | `instance?`, `state` (on/off/toggle)   | `Application.SetMute` |
  | `speed`     | `instance?`, `speed` (−32…32, or `increment`/`decrement`) | `Player.SetSpeed` — fast-forward / rewind / resume normal |
  | `skip`      | `instance?`, `to` (`next`/`previous`, or item position) | `Player.GoTo` |
  | `shuffle`   | `instance?`, `state` (on/off/toggle)   | `Player.SetShuffle` |
  | `repeat`    | `instance?`, `mode` (off/one/all/cycle) | `Player.SetRepeat` |
  | `track`     | `instance?`, `kind` (audio/subtitle), `select` (index, `next`/`previous`, `on`/`off`), `path?` | `Player.SetAudioStream` / `Player.SetSubtitle` / `Player.AddSubtitle` |
  | `queue`     | `instance?`, `action` (add/insert/remove/clear/list), `type?`, `id?`, `position?` | `Playlist.Add`/`Insert`/`Remove`/`Clear`/`GetItems` |
  | `recent`    | `type` (movies/episodes/albums/songs), `played?` (added vs played), `limit?` | `VideoLibrary.GetRecentlyAdded*` / `AudioLibrary.GetRecentlyAdded*`/`GetRecentlyPlayed*` |
  | `continue`  | `instance?`                            | `VideoLibrary.GetInProgressTVShows` — continue watching |
  | `genres`    | `type` (movie/tvshow/music)            | `VideoLibrary.GetGenres` / `AudioLibrary.GetGenres` |
  | `navigate`  | `instance?`, `action` (up/down/left/right/select/back/home/contextmenu/info/osd) | `Input.*` |
  | `text`      | `instance?`, `text`, `done?`           | `Input.SendText` — type into the focused field |
  | `screen`    | `instance?`, `window?` (home/visualisation/…), `fullscreen?` | `GUI.ActivateWindow` / `GUI.SetFullscreen` |
  | `power`     | `instance?`, `action` (shutdown/reboot/suspend/hibernate/quit/ejectoptical) | `System.*` / `Application.Quit` |
  | `playfile`  | `instance?`, `file`                    | `Player.Open` with a `{file}` item (§12.10.9) — plays the `file` from a `search` result; works for any path, in-library or not |
  | `files`     | `instance?`, `path?` (a source/dir; omitted → sources) | `Files.GetSources` / `Files.GetDirectory` |
  | `rpc`       | `instance?`, `method`, `params?` (object) | passthrough — any JSON-RPC method; returns Kodi's **raw** `result` unshaped. Opt-in per instance (`allow_rpc`, §7.7) — off by default, refuses otherwise |

  [ ] 5.3 `random` and `search` resolve library ids internally (e.g. a show name
  → `tvshowid` → episodes) so the AI can act by name, not just numeric id.
  [ ] 5.4 **`status` with `instance: "*"`** queries every configured instance and
  returns a per-instance now-playing summary (item, position, paused/playing) —
  this answers "what's playing everywhere" / "what are the children watching"
  (target the `kids` instance, or `*` and read its row).
  [ ] 5.5 **`handoff`** moves playback between instances: read the active player's
  item id/type and resumable position (`Player.GetProperties` → `time`/
  `percentage`) on `from`, stop `from`, `Player.Open` the same item on `to`, then
  `seek` `to` to that position. Records the move in both instances' state (§8).
  Built on the `seek` primitive; report a clear error if `from` has nothing
  playing.
  [ ] 5.6 **`handoff` assumes a shared library:** it re-opens on `to` by the item
  id captured from `from`, which is only valid when both boxes resolve that id to
  the same content (a shared library / common MySQL backend, or identical local
  libraries). The server does not blindly stop `from` and hope: before stopping,
  confirm the item is reachable on `to` (verify the id resolves there, e.g.
  `VideoLibrary.GetEpisodeDetails` / `AudioLibrary.Get*Details`). If it is **not
  available at `to`**, abort the handoff with a clear error and leave `from`
  playing untouched — never stop one box for a move that cannot complete.
  [ ] 5.7 **Player-targeted tools resolve the active player first.** `speed`,
  `skip`, `shuffle`, `repeat`, `track` act on whatever `Player.GetActivePlayers`
  reports for the instance; with nothing playing they return a clean tool error
  (`isError`), never a protocol error (§3.4). `queue` operates on the playlist
  whose type matches the queued item (`audio` vs `video`).
  [ ] 5.8 **`power` is destructive.** `shutdown`/`reboot`/`suspend`/`hibernate`/
  `quit` end the session or power down the box; the schema description must say so
  plainly so the model only invokes them on an explicit request. No extra
  confirmation flow in the server — the MCP client is responsible for user
  approval — but the description should not read as routine.
  [ ] 5.9 **Library-browse tools** (`recent`, `continue`, `genres`) return id +
  title rows the same shape `search`/`episodes` use, so the AI can chain a browse
  result straight into `play`/`queue` by id (§5.3).
  [ ] 5.10 **Generic `search` → `playfile` (find then play).** One `search` tool
  spans all three media types and drills its per-type hierarchy down to playable
  **leaf files**; `playfile` then plays a `file` from the result. Verified against
  a live Kodi 20.5 — the captures backing each step are in §12 (linked below).

  Fields (all optional except `type`; an LLM fills only what the user named):

  | Field    | music                       | tv-show                  | movie                |
  |----------|-----------------------------|--------------------------|----------------------|
  | `type`   | `"music"`                   | `"tv-show"`              | `"movie"`            |
  | `artist` | performer (filters artist)  | —                        | —                    |
  | `title`  | album name                  | show name                | movie title          |
  | `season` | —                           | season number            | —                    |
  | `number` | track number                | episode number           | —                    |
  | `limit`  | max leaf rows returned (default cap, e.g. 50)                  |||
  | `offset` | rows to skip — paginate together with `limit`                 |||
  | `count`  | if true, return `total` only (zero rows) — a cheap count      |||

  `title` is the one renamed field (was "group"): it holds the named container
  for music/tv (album/show) and the title itself for a movie — the thing you
  search by. `number` is the position within a group (track or episode). All
  text matching is substring + case-insensitive (`filter {field, operator:
  "contains", value}`), so `"abba"` also hits "Black Sa**bba**th" — narrow with
  more fields.

  Resolution chains (each step = a §12 method already captured). The key design
  choice: **descend to the album/season level only when the user names one**, so
  every *wide* query (bare `artist`, bare show) is a **single** leaf call — which
  makes `total`, `limit`, and `offset` map straight onto Kodi's `limits {start,
  end, total}` with no app-side stitching:
  - **music** — `GetArtists`(`artist`) → `artistid`. Then:
    - no `title` → `GetSongs {artistid}` **directly** — every song by the artist
      in one call (the §12.3.6 shortcut; `GetSongs` takes an `{artistid}` filter,
      introspect.json:1132). Skips the album hop entirely.
    - with `title` → `GetAlbums {artistid}`(+`title`) → `albumid` →
      `GetSongs {albumid}` (+`number`→track). (§12.3.4 / §12.3.17.)
  - **tv-show** — `GetTVShows`(`title`) → `tvshowid` → `GetEpisodes {tvshowid,
    season?}` (+`number`→episode filter) → episode `file`s, one call. (§12.16.21
    incl. the episode follow-up; §12.16.6.)
  - **movie** — `GetMovies`(`title`) → movie `file`s directly (no sublevels),
    one call. (§12.16.12.)

  **Leaf-files semantics** (the chosen behaviour): `search` always returns the
  leaf rows it can reach with the fields given — never a bare container. Omitting
  lower fields *widens* to all leaves beneath:
  - `{music, artist:"Abba"}` → every Abba song (all albums, one `GetSongs` call).
  - `{music, artist:"Abba", title:"Arrival"}` → every track of Arrival.
  - `{music, artist:"Abba", title:"Arrival", number:3}` → just track 3.
  - `{tv-show, title:"Earth 2"}` → every episode; `+season:1` → that season;
    `+number:2` → exactly S01E02.
  - `{movie, title:"Alien"}` → the matching movie file(s).
  Each result row carries at least `file` (the `playfile` input) plus its library
  id (`songid`/`episodeid`/`movieid`) and a human `label`/`title`.

  **Count and pagination.** Every Get\* response includes `limits.total` — the
  full match count, independent of how many rows were returned. `search` always
  surfaces that as `total`, so the count is honest even when rows are capped:
  "how many Iron Maiden songs?" is `{music, artist:"Iron Maiden", count:true}` →
  one `GetSongs {artistid}` with `limits {start:0, end:1}`, read `total`, return
  zero rows. To build a long playlist, page with `limit`/`offset` (→ Kodi
  `limits {start:offset, end:offset+limit}`) until `offset+len(rows) >= total`.
  When `limit` is omitted the default cap applies and the response flags
  truncation (`returned < total`). A bare `artist` can legitimately be hundreds
  of songs (real data: one artist had 65 albums, §12.3.6) — `total` tells the
  caller that up front; `limit`/`offset` walk it.

  **The one multi-call case:** a substring `title` that matches *several* albums
  or shows (e.g. two albums both containing "Live") forces a `GetSongs`/
  `GetEpisodes` per match, so `total` is a **sum** across them and `offset` is
  best-effort (sliced app-side over the concatenated leaves). The common paths —
  bare `artist`, bare show, exact-ish `title` resolving to one container — stay
  single-call with exact `limits`. Flag this set as approximate when it occurs.

  `playfile { file }` feeds that `file` to `Player.Open {item:{file}}` (§12.10.9):
  plays any path (no library entry needed), auto-selects the audio vs video
  player from the file type, and — when the file matches a scanned item — Kodi
  enriches the now-playing state back to the library id. Plays **one** file; for
  multi-leaf results the caller picks which (a future "queue all" path → §queue).
  Returns the post-open `player_state()` snapshot, like the transport Buttons.

  Notes / gotchas surfaced while exploring (all in §12): the episode-number
  filter `value` is a **string**; `playerid` for follow-up control is not fixed
  (audio=0, video=1) — read it from `GetActivePlayers`; scraped `title`/`label`
  can differ from the on-disk filename; duplicate albums and malformed/empty tags
  exist in real libraries, so dedupe/guard. This generic `search` subsumes the
  separate `episodes` row (§5.2) — fold or keep as a thin alias (decide later).

---

## 6. Source layout

  [ ] 6.1 Lay out the tree as below; prefix `mk_` / `Mk` for our own symbols.

  ```
  mcp-kodi/
  ├── configure.ac          autotools: deps, output
  ├── Makefile.am           top-level build → src/
  ├── autogen.sh            autoreconf bootstrap
  ├── README.md             what it is + MCP client config snippet
  ├── TODO.md               this file
  └── src/
      ├── Makefile.am       builds the mcp-kodi binary
      ├── main.c            entry: load config, set up GMainLoop + stdio, run
      ├── mk-config.{c,h}   load/save ~/.config/mcp-kodi/config.json + env
      ├── mk-stdio.{c,h}    newline-delimited JSON-RPC read/write over stdin/out
      ├── mk-mcp.{c,h}      MCP method dispatch (initialize/list/call/ping)
      ├── mk-kodi.{c,h}     Kodi JSON-RPC client over libsoup
      ├── mk-tools.{c,h}    tool table: schemas + handlers → mk-kodi calls
      ├── mk-state.{c,h}    playback state file (json, atomic write)
      └── mk-history.{c,h}  playback history log — global, append, atomic (§13)
  ```

---

## 7. Configuration file

  [x] 7.1 **Location:** `${XDG_CONFIG_HOME:-~/.config}/mcp-kodi/config.json` — the
  server's own file. No `cli`/shell config in this project; JSON so it round-trips
  through the json-glib we already use and stays symmetric with the state file
  ([§8](#8-playback-state-file)).
  [x] 7.2 **Multiple instances:** the server can drive several Kodi boxes (e.g.
  `hall`, `bedroom`, `kids`). Config holds a map of named instances and names one
  `default`. Shape (draft):
  ```json
  {
    "version": 2,
    "default": "hall",
    "instances": {
      "hall":    { "name": "Living Room TV", "host": "hall.example.local:8443",    "auth": "kodi:<password>", "scheme": "https", "insecure": true, "allow_rpc": true },
      "bedroom": { "name": "Bedroom",        "host": "bedroom.example.local:8443", "auth": "kodi:<password>", "scheme": "https", "insecure": true },
      "kids":    { "name": "Kids' Room",     "host": "kids.example.local:8443",    "auth": "kodi:<password>", "scheme": "https", "insecure": true }
    }
  }
  ```
  (`auth` is `user:pass` for HTTP Basic; `insecure: true` accepts the self-signed
  cert — the JSON equivalent of curl `-k`. The instance *key* (`hall`, `bedroom`,
  …) is the short identifier tools reference, and `default` is used when a tool
  omits `instance`. The optional `name` is a free-form human-readable display
  label — surfaced in the tool schema so the assistant can refer to a box by its
  friendly name — and is omitted from the file when unset. The optional
  `allow_rpc: true` opts that instance into the generic `rpc` escape hatch
  (§7.7); absent/false → that box rejects `rpc`.)
  [x] 7.3 **Load:** on startup read the file if present. Environment overrides
  apply to the `default` instance only — `KODI_HOST`/`KODI_AUTH`/`KODI_SCHEME`
  (and `-k` in `KODI_CURL_OPTS` → `insecure`) — so a single-box user can run with
  no config file at all (env defines one implicit `default` instance). No file and
  no env → a clear error telling the user to configure. Never hardcode host or
  credentials.
  [x] 7.4 **Save:** write the effective config back atomically — temp file in the
  same dir, `fsync`, then `rename()` over the target (same discipline as the state
  file, §8.4). Create the directory `0700` and the file `0600`; it holds
  passwords.
  [ ] 7.5 **Save trigger:** the save primitive lives in `mk-config` regardless of
  caller; the entry point is the `instances` config tool (§11.6.3) — its `set`
  and `remove` actions write the file back atomically (§7.4).
  [x] 7.6 **Back-compat:** a `version: 1` flat file (single `host`/`auth`/… at the
  top level) is read as one instance named `default`; on next save it is rewritten
  in the `version: 2` instances shape.
  [x] 7.7 **`rpc` escape hatch is opt-in, per instance.** The generic `rpc`
  passthrough (§5.2, §11.6.6) can invoke *any* JSON-RPC method on a box —
  unconstrained and powerful — so it is **disabled by default**. An instance
  permits it only when its config object carries `"allow_rpc": true`; absent or
  `false` → the `rpc` tool refuses for that instance with a clean tool error
  (§3.4). The flag is granted **only by hand-editing `config.json`**, separately
  for **each** instance — it is intentionally *not* among the fields the
  `instances` tool can write (§11.6.3.2), so the assistant can never enable its
  own escape hatch; opting a box in is an explicit, out-of-band human decision.
  `instances` `get` surfaces it read-only (an `allow_rpc` boolean per instance)
  so the caller can see where it is permitted without being able to change it.

---

## 8. Playback state file

  [ ] 8.1 **Purpose:** remember, across stateless calls and *per instance*, *what
  we started to play* and *the last episode watched per TV show*, so the AI can
  resume / continue / hand off between boxes.
  [ ] 8.2 **Location:** one state file **per instance**, under a state directory
  `${XDG_STATE_HOME:-~/.local/state}/mcp-kodi/instances/`, named for the instance
  (e.g. `hall.json`, `bedroom.json`). The configuration file ([§7](#7-configuration-file))
  holds **no** state — config and state live in separate trees so a config edit
  never races a state write and vice versa. Instance names are simple labels;
  percent-encode any byte outside `[A-Za-z0-9._-]` to keep filenames safe.
  [ ] 8.3 **Shape (draft):** each file is one instance's state on its own — no
  instance keying, no mirror of the config map:
  ```json
  {
    "version": 1,
    "last_played": { "type": "episode", "id": 1234, "label": "...", "position": "00:12:34", "at": "<iso8601>" },
    "shows": { "<tvshowid>": { "last_episode_id": 5678, "season": 3, "episode": 7, "at": "<iso8601>" } }
  }
  ```
  An instance with nothing recorded yet has `last_played: null` and `shows: {}`
  (or simply no file).
  [ ] 8.4 **Writes:** on `play`/`random`/`handoff` for the target instance, write
  *that instance's* file (record `last_played`; if it's an episode, update its
  `shows` entry). `handoff` writes both the source and destination files, and
  captures the resumed `position` on the destination. Atomic per file: write a
  temp file in the same dir, `fsync`, then `rename()` over the target. Create the
  `instances/` dir (0700) on first write.
  [ ] 8.5 **Reads:** available to handlers that want "what next" context, scoped
  to one instance by reading that instance's file; may later back a `resume` /
  `next_episode` tool. Keep files optional — a missing file = empty state for that
  instance.
  [ ] 8.6 **Back-compat:** a legacy single `state.json` (the earlier draft's
  instance-keyed file) is split into per-instance files on first run — one
  `instances/<name>.json` per key — then the old file is left in place but
  ignored. No on-disk state has shipped, so this migration is best-effort.

---

## 9. Build/runtime requirements

  [ ] 9.1 Deps (all present on the dev box): `glib-2.0 >= 2.80`, `gobject-2.0`,
  `gio-2.0`, `json-glib-1.0 >= 1.8`, `libsoup-3.0 >= 3.4`.
  [ ] 9.2 Toolchain: gcc 13, autotools, `pkg-config`.
  [ ] 9.3 Warnings-as-errors during dev (`-Wall -Wextra`); check LSP diagnostics
  after each edit.

---

## 10. Future goals (not yet designed)

  Parked ideas — recorded so the design stays compatible with them, not committed
  work.

  [ ] 10.1 **Auto-continue / next-episode playback:** start a TV episode and, when
  it finishes, automatically play the next episode of the show — binge a season
  hands-off. The hard part is *noticing* the episode ended **without** a
  long-lived monitoring loop: this server is built on stateless, self-contained
  `tools/call`s (§2), so we explicitly do not want a persistent background session
  polling the player. Approaches to weigh later: subscribe to Kodi's own
  `Player.OnStop` / `Player.OnAVEnd` push notifications over the WebSocket channel
  (push, not poll); or build a transient Kodi-side playlist/queue so Kodi advances
  itself; or a lightweight external timer keyed to remaining runtime. The
  per-instance `shows` history in state (§8) already records the last episode
  watched, which seeds "what plays next". Design TBD.
  [ ] 10.2 **Watched / resume awareness:** remember what has been watched and how
  far. Kodi already tracks most of this in its own video library — per item
  `playcount` (watched vs unwatched), `lastplayed`, and a `resume` object
  (`position`/`total` seconds), all readable via `VideoLibrary.Get*Details` /
  `GetEpisodes`. So the server should *read Kodi's native watched/resume data*
  rather than duplicate it in our state file (§8); our state stays for
  cross-instance intent (handoff, "what next"). The harder piece is the Kodi
  **GUI navigation memory** — the UI remembers the last show → season → episode
  you browsed to (enter/enter/enter lands on it), which isn't the same as library
  watched-state and isn't cleanly exposed over JSON-RPC; reproducing "take me back
  to where I was in the menus" needs its own approach. Design TBD.

---

## 11. Status / next steps

  [x] 11.1 Spec (this file)
  [x] 11.2 Autotools scaffold (configure.ac, Makefile.am, autogen.sh, src
  skeleton)
  [x] 11.3 Config load + save, multi-instance (`mk-config`)
  [x] 11.4 Kodi JSON-RPC client, per-instance (`mk-kodi`)
  [x] 11.5 MCP stdio transport + dispatch (`mk-stdio`, `mk-mcp`)
  [ ] 11.6 Design tool tables
    [ ] 11.6.1 Buttons — argument-free actions; modeled as remote keypresses via
    Input.ExecuteAction (no playerid, no active-player resolution):
      [x] 11.6.1.1 play
        `{"method":"Input.ExecuteAction","params":{"action":"play"}}`
      [x] 11.6.1.2 pause
        `{"method":"Input.ExecuteAction","params":{"action":"pause"}}`
      [x] 11.6.1.3 stop
        `{"method":"Input.ExecuteAction","params":{"action":"stop"}}`
      [x] 11.6.1.4 mute
        `{"method":"Application.SetMute","params":{"mute":true}}`
      [x] 11.6.1.5 unmute
        `{"method":"Application.SetMute","params":{"mute":false}}`
      [x] 11.6.1.6 noop — fires no action; just returns the player_state()
        snapshot. A reachability + state probe: the caller uses it to learn
        whether Kodi answers and what is currently loaded/playing, without
        changing anything (no Input.ExecuteAction). Like the transport Buttons
        it shares the player_state() response shape.
    [ ] 11.6.2 Knobs — set to a scalar value; $value = the knob argument:
      [ ] 11.6.2.1 volume
        `{"method":"Application.SetVolume","params":{"volume":"$value"}}`
    [x] 11.6.3 Config tools — manage the server's *own* instance config
    (`config.json`, §7); these make no Kodi call. One `instances` tool with an
    `action` enum (the §5.2 convention used by `queue`/`power`/`navigate`). `set`
    is the save trigger left TBD in §7.5. Every action returns the resulting
    instance list so the caller sees the new state; credentials are **never**
    returned (`auth` is reported only as a boolean `has_auth`).
      [x] 11.6.3.1 get — `{"action":"get"}`. List configured instances, each as
        `{ key, name?, host, scheme, insecure, has_auth, allow_rpc }`, plus which
        `key` is `default`. `allow_rpc` is reported read-only (§7.7). No password
        ever leaves the server.
        (`mk_config_instance_names` + `mk_config_get_instance` +
        `mk_config_get_default`.)
      [x] 11.6.3.2 set — `{"action":"set","key":…, "name"?,"host"?,"auth"?,`
        `"scheme"?,"insecure"?,"default"?}`. Upsert one instance by `key`:
        provided fields override, omitted fields keep the existing value (or take
        defaults for a new instance), then persist atomically (§7.4). `default:true`
        makes it the default. `allow_rpc` is deliberately **not** writable here —
        the escape-hatch gate is hand-edit-only (§7.7), so this action neither
        accepts nor clears it. (`mk_config_get_instance` to merge →
        `mk_config_set_instance` → `mk_config_set_default` → `mk_config_save`.)
      [x] 11.6.3.3 remove — `{"action":"remove","key":…}`. Delete the named
        instance and save. Refuse with a clean tool error if `key` is the current
        `default` (caller must `set` a different `default` first), so the config
        is never left defaultless/empty. (Needs a new `mk_config_remove_instance`
        in the config API.)
    [ ] 11.6.4 Search tools — resolve the library by name down to playable leaf
    files; no Kodi state change. Generic find-by-name tools (more to come).
    Full design in §5.10.
      [x] 11.6.4.1 search — `{type:music|tv-show|movie, artist?, title?, season?,`
        `number?, limit?, offset?, count?}`. Drills the per-type chain to
        leaf-file rows (`{file, <id>, label}`): music GetArtists→GetSongs
        `{artistid}` direct when no `title`, else GetArtists→GetAlbums→GetSongs
        `{albumid}` (§12.3.6/§12.3.4/§12.3.17); tv-show GetTVShows→GetEpisodes
        (§12.16.21/§12.16.6); movie GetMovies (§12.16.12). Descend to album/season
        only when named, so each wide query is one Kodi call. Omitting lower
        fields widens to all leaves beneath. Always report `total` (from Kodi
        `limits.total`); `count:true` returns `total` with zero rows. `limit`/
        `offset` → Kodi `limits {start:offset, end:offset+limit}` for paging a
        long result (e.g. playlist build); default cap applies when `limit`
        omitted, flagging truncation (`returned < total`). `title` =
        album/show/movie name; `number` = track/episode. A substring `title`
        hitting several containers is the one multi-call case → `total` is a sum,
        `offset` best-effort, flag approximate (§5.10).
    [x] 11.6.5 playfile — `{file}`. `Player.Open {"item":{"file":…}}` (§12.10.9):
    plays any path (in-library or not), auto-selects the audio/video player,
    library-enriches the now-playing item; returns the player_state() snapshot.
    Plays one file (caller picks from a multi-leaf `search` result).
    [x] 11.6.6 rpc — escape hatch. `{instance?, method, params?}`. Build the
    JSON-RPC envelope (§4.1) and POST it to the resolved instance (§4.2), then
    return Kodi's **raw `result` verbatim** — no shaping or summarising, unlike
    every other tool (the §2.3 generic hatch for methods §5/§11.6 don't model;
    `params` defaults to `{}` when omitted). A Kodi `error` member surfaces as a
    tool error (§3.4/§4.5), not a protocol error. **Gated per instance:** refuses
    unless that instance's config sets `allow_rpc: true` (§7.7) — off by default,
    enabled only by hand-editing `config.json`, never by the `instances` tool, so
    the model cannot grant itself the hatch. An instance without the flag returns
    a clean tool error naming the instance and the flag; the schema lists which
    configured instances currently permit `rpc`.
  [ ] 11.7 Prompts / resources — **deferred, not shipping now.** Neither MCP
    `prompts/*` nor `resources/*` is implemented; tools (§5/§11.6) are the whole
    server surface for now. `initialize` advertises only `{ "tools": {} }`
    (mk-mcp.c). Prompts were designed (workflow templates) but pulled — they
    mostly reference tools not yet built (`status`/`continue`/`episodes`/
    `screen`) so there's little to list, and resources duplicate existing tools
    or need the monitoring loop §2.1/§10.1 avoids. Revisit once more of the §5.2
    tool surface lands.
  [ ] 11.8 Playback state file, per-instance (`mk-state`)
  [ ] 11.9 Playback history, global append-only log (`mk-history`) — see §13
  [ ] 11.10 Build clean, test against live Kodi, write README

## 12. Comprehensive Kodi JSON-RPC coverage

  One item per JSON-RPC method (Kodi 19.4, API v12.4.0 — see
  `docs/kodi-jsonrpc-catalog.md` for full params/types). This is the raw
  method inventory; §5/§11.6 decide how methods are grouped into MCP tools.

  [ ] 12.1 Application
    [ ] 12.1.1 properties — Retrieves the values of the given properties (`Application.GetProperties`)
    [ ] 12.1.2 quit — Quit application (`Application.Quit`)
    [ ] 12.1.3 mute — Toggle mute/unmute (`Application.SetMute`)
    [ ] 12.1.4 volume — Set the current volume (`Application.SetVolume`)

  [ ] 12.2 Addons
    [ ] 12.2.1 executeAddon — Executes the given addon with the given parameters (if possible) (`Addons.ExecuteAddon`)
    [ ] 12.2.2 addonDetails — Gets the details of a specific addon (`Addons.GetAddonDetails`)
    [ ] 12.2.3 addons — Gets all available addons (`Addons.GetAddons`)
    [ ] 12.2.4 addonEnabled — Enables/Disables a specific addon (`Addons.SetAddonEnabled`)

  [ ] 12.3 AudioLibrary
    [ ] 12.3.1 clean — Cleans the audio library from non-existent items (`AudioLibrary.Clean`)
    [ ] 12.3.2 export — Exports all items from the audio library (`AudioLibrary.Export`)
    [ ] 12.3.3 albumDetails — Retrieve details about a specific album (`AudioLibrary.GetAlbumDetails`)
    [x] 12.3.4 albums — Retrieve all albums from specified artist (and role) or that has songs of the specified genre (`AudioLibrary.GetAlbums`) — see chain in 12.3.6 (step 2): `{"artistid": N}` or `{"genreid": N}` filter.
    [ ] 12.3.5 artistDetails — Retrieve details about a specific artist (`AudioLibrary.GetArtistDetails`)
    [x] 12.3.6 artists — Retrieve all artists. For backward compatibility by default this implicitly does not include those that only contribute other roles, however absolutely all artists can be returned using allroles=true (`AudioLibrary.GetArtists`)
        Music is a 3-level hierarchy and needs up to three calls to reach a
        playable file: artist → album → song. Same `filter`/`sort`/`limits`/
        `properties` shape as the video Get* methods. The chain below drills from
        an artist name down to the song `file`.

        Step 1 — find the artist (`AudioLibrary.GetArtists`):
        ```json
        {
          "jsonrpc": "2.0",
          "id": 1,
          "method": "AudioLibrary.GetArtists",
          "params": {
            "filter": { "field": "artist", "operator": "contains", "value": "abba" },
            "properties": ["genre"],
            "sort": { "method": "artist", "order": "ascending" }
          }
        }
        ```
        ```json
        {
          "id": 1,
          "jsonrpc": "2.0",
          "result": {
            "limits": { "start": 0, "end": 2, "total": 2 },
            "artists": [
              { "artistid": 3,  "artist": "Abba",          "label": "Abba",          "genre": [] },
              { "artistid": 31, "artist": "Black Sabbath", "label": "Black Sabbath", "genre": [] }
            ]
          }
        }
        ```
        Note: `contains` is a substring match — `"abba"` also matched
        "Black Sa**bba**th". `artistid` carries into step 2.

        Step 2 — albums of that artist (`AudioLibrary.GetAlbums`, see 12.3.4).
        Use the special `{"artistid": N}` filter (not a field/operator rule):
        ```json
        {
          "jsonrpc": "2.0",
          "id": 2,
          "method": "AudioLibrary.GetAlbums",
          "params": {
            "filter": { "artistid": 3 },
            "properties": ["title", "year", "artist"],
            "sort": { "method": "year", "order": "ascending" },
            "limits": { "start": 0, "end": 5 }
          }
        }
        ```
        ```json
        {
          "id": 2,
          "jsonrpc": "2.0",
          "result": {
            "limits": { "start": 0, "end": 5, "total": 65 },
            "albums": [
              { "albumid": 4, "title": "Arrival", "label": "Arrival", "year": 0, "artist": ["Arrival (Digitally Remastered)"] },
              { "albumid": 9, "title": "Arrival", "label": "Arrival", "year": 0, "artist": ["Abba"] },
              { "albumid": 8, "title": "Ring Ring", "label": "Ring Ring", "year": 0, "artist": ["Abba"] }
            ]
          }
        }
        ```
        Data caveats (real library): `total` 65 because three overlapping ABBA
        discography folders created duplicate albums (e.g. "Arrival" = albumid 4
        and 9); `year` is 0 when tags lack it; one album's `artist` is a malformed
        tag ("Arrival (Digitally Remastered)"). `albumid` carries into step 3.

        Step 3 — songs of that album = the file level (`AudioLibrary.GetSongs`,
        see 12.3.17). Special `{"albumid": N}` filter; request `file`:
        ```json
        {
          "jsonrpc": "2.0",
          "id": 3,
          "method": "AudioLibrary.GetSongs",
          "params": {
            "filter": { "albumid": 8 },
            "properties": ["title", "track", "duration", "artist", "album", "file"],
            "sort": { "method": "track", "order": "ascending" },
            "limits": { "start": 0, "end": 3 }
          }
        }
        ```
        ```json
        {
          "id": 3,
          "jsonrpc": "2.0",
          "result": {
            "limits": { "start": 0, "end": 3, "total": 31 },
            "songs": [
              { "songid": 86, "track": 1, "title": "Ring Ring",                   "duration": 184, "artist": ["Abba"], "album": "Ring Ring", "file": "/media/Music/ABBA - Discography 1973-2006 Mp3 320 kbps/2005 The Complete Studio Recordings/1973 Ring Ring @320/01 Ring Ring.mp3" },
              { "songid": 87, "track": 2, "title": "Another Town, Another Train", "duration": 192, "artist": ["Abba"], "album": "Ring Ring", "file": "/media/Music/ABBA - Discography 1973-2006 Mp3 320 kbps/2005 The Complete Studio Recordings/1973 Ring Ring @320/02 Another Town, Another Train.mp3" },
              { "songid": 88, "track": 3, "title": "Disillusion",                 "duration": 185, "artist": ["Abba"], "album": "Ring Ring", "file": "/media/Music/ABBA - Discography 1973-2006 Mp3 320 kbps/2005 The Complete Studio Recordings/1973 Ring Ring @320/03 Disillusion.mp3" }
            ]
          }
        }
        ```
        `songid` is the `Player.Open {songid}` handle; `file` is the playable path;
        `duration` is seconds. Shortcuts: skip levels with `GetSongs`
        `{"artistid": N}` (all songs by an artist) or `{"genreid": N}`; albums also
        accept `{"genreid": N}`. `title` field filter works at every level too.
    [ ] 12.3.7 availableArt — Retrieve all potential art URLs for a media item by art type (`AudioLibrary.GetAvailableArt`)
    [ ] 12.3.8 availableArtTypes — Retrieve a list of potential art types for a media item (`AudioLibrary.GetAvailableArtTypes`)
    [ ] 12.3.9 genres — Retrieve all genres (`AudioLibrary.GetGenres`)
    [ ] 12.3.10 properties — Retrieves the values of the music library properties (`AudioLibrary.GetProperties`)
    [ ] 12.3.11 recentlyAddedAlbums — Retrieve recently added albums (`AudioLibrary.GetRecentlyAddedAlbums`)
    [ ] 12.3.12 recentlyAddedSongs — Retrieve recently added songs (`AudioLibrary.GetRecentlyAddedSongs`)
    [ ] 12.3.13 recentlyPlayedAlbums — Retrieve recently played albums (`AudioLibrary.GetRecentlyPlayedAlbums`)
    [ ] 12.3.14 recentlyPlayedSongs — Retrieve recently played songs (`AudioLibrary.GetRecentlyPlayedSongs`)
    [ ] 12.3.15 roles — Retrieve all contributor roles (`AudioLibrary.GetRoles`)
    [ ] 12.3.16 songDetails — Retrieve details about a specific song (`AudioLibrary.GetSongDetails`)
    [x] 12.3.17 songs — Retrieve all songs from specified album, artist or genre (`AudioLibrary.GetSongs`) — see chain in 12.3.6 (step 3): `{"albumid": N}`/`{"artistid": N}`/`{"genreid": N}` filter; `file` = playable path, `songid` = Player.Open handle.
    [ ] 12.3.18 sources — Get all music sources, including unique ID (`AudioLibrary.GetSources`)
    [ ] 12.3.19 scan — Scans the audio sources for new library items (`AudioLibrary.Scan`)
    [ ] 12.3.20 setAlbumDetails — Update the given album with the given details (`AudioLibrary.SetAlbumDetails`)
    [ ] 12.3.21 setArtistDetails — Update the given artist with the given details (`AudioLibrary.SetArtistDetails`)
    [ ] 12.3.22 setSongDetails — Update the given song with the given details (`AudioLibrary.SetSongDetails`)

  [ ] 12.4 Favourites
    [ ] 12.4.1 addFavourite — Add a favourite with the given details (`Favourites.AddFavourite`)
    [ ] 12.4.2 favourites — Retrieve all favourites (`Favourites.GetFavourites`)

  [ ] 12.5 Files
    [ ] 12.5.1 directory — Get the directories and files in the given directory (`Files.GetDirectory`)
    [ ] 12.5.2 fileDetails — Get details for a specific file (`Files.GetFileDetails`)
    [ ] 12.5.3 sources — Get the sources of the media windows (`Files.GetSources`)
    [ ] 12.5.4 prepareDownload — Provides a way to download a given file (e.g. providing an URL to the real file location) (`Files.PrepareDownload`)
    [ ] 12.5.5 setFileDetails — Update the given specific file with the given details (`Files.SetFileDetails`)

  [ ] 12.6 GUI
    [ ] 12.6.1 activateWindow — Activates the given window (`GUI.ActivateWindow`)
    [ ] 12.6.2 properties — Retrieves the values of the given properties (`GUI.GetProperties`)
    [ ] 12.6.3 stereoscopicModes — Returns the supported stereoscopic modes of the GUI (`GUI.GetStereoscopicModes`)
    [ ] 12.6.4 fullscreen — Toggle fullscreen/GUI (`GUI.SetFullscreen`)
    [ ] 12.6.5 stereoscopicMode — Sets the stereoscopic mode of the GUI to the given mode (`GUI.SetStereoscopicMode`)
    [ ] 12.6.6 showNotification — Shows a GUI notification (`GUI.ShowNotification`)

  [ ] 12.7 Input
    [ ] 12.7.1 back — Goes back in GUI (`Input.Back`)
    [ ] 12.7.2 buttonEvent — Send a button press event (`Input.ButtonEvent`)
    [ ] 12.7.3 contextMenu — Shows the context menu (`Input.ContextMenu`)
    [ ] 12.7.4 down — Navigate down in GUI (`Input.Down`)
    [ ] 12.7.5 executeAction — Execute a specific action (`Input.ExecuteAction`)
    [ ] 12.7.6 home — Goes to home window in GUI (`Input.Home`)
    [ ] 12.7.7 info — Shows the information dialog (`Input.Info`)
    [ ] 12.7.8 left — Navigate left in GUI (`Input.Left`)
    [ ] 12.7.9 right — Navigate right in GUI (`Input.Right`)
    [ ] 12.7.10 select — Select current item in GUI (`Input.Select`)
    [ ] 12.7.11 sendText — Send a generic (unicode) text (`Input.SendText`)
    [ ] 12.7.12 showCodec — Show codec information of the playing item (`Input.ShowCodec`)
    [ ] 12.7.13 showOSD — Show the on-screen display for the current player (`Input.ShowOSD`)
    [ ] 12.7.14 showPlayerProcessInfo — Show player process information of the playing item, like video decoder, pixel format, pvr signal strength, ... (`Input.ShowPlayerProcessInfo`)
    [ ] 12.7.15 up — Navigate up in GUI (`Input.Up`)

  [ ] 12.8 JSONRPC
    [ ] 12.8.1 introspect — Enumerates all actions and descriptions (`JSONRPC.Introspect`)
    [ ] 12.8.2 notifyAll — Notify all other connected clients (`JSONRPC.NotifyAll`)
    [ ] 12.8.3 permission — Retrieve the clients permissions (`JSONRPC.Permission`)
    [ ] 12.8.4 ping — Ping responder (`JSONRPC.Ping`)
    [ ] 12.8.5 version — Retrieve the JSON-RPC protocol version. (`JSONRPC.Version`)

  [ ] 12.9 PVR
    [ ] 12.9.1 addTimer — Adds a timer to record the given show one times or a timer rule to record all showings of the given show or adds a reminder timer or reminder timer rule (`PVR.AddTimer`)
    [ ] 12.9.2 deleteTimer — Deletes a onetime timer or a timer rule (`PVR.DeleteTimer`)
    [ ] 12.9.3 broadcastDetails — Retrieves the details of a specific broadcast (`PVR.GetBroadcastDetails`)
    [ ] 12.9.4 broadcastIsPlayable — Retrieves whether or not a broadcast is playable (`PVR.GetBroadcastIsPlayable`)
    [ ] 12.9.5 broadcasts — Retrieves the program of a specific channel (`PVR.GetBroadcasts`)
    [ ] 12.9.6 channelDetails — Retrieves the details of a specific channel (`PVR.GetChannelDetails`)
    [ ] 12.9.7 channelGroupDetails — Retrieves the details of a specific channel group (`PVR.GetChannelGroupDetails`)
    [ ] 12.9.8 channelGroups — Retrieves the channel groups for the specified type (`PVR.GetChannelGroups`)
    [ ] 12.9.9 channels — Retrieves the channel list (`PVR.GetChannels`)
    [ ] 12.9.10 clients — Retrieves the enabled PVR clients and their capabilities (`PVR.GetClients`)
    [ ] 12.9.11 properties — Retrieves the values of the given properties (`PVR.GetProperties`)
    [ ] 12.9.12 recordingDetails — Retrieves the details of a specific recording (`PVR.GetRecordingDetails`)
    [ ] 12.9.13 recordings — Retrieves the recordings (`PVR.GetRecordings`)
    [ ] 12.9.14 timerDetails — Retrieves the details of a specific timer (`PVR.GetTimerDetails`)
    [ ] 12.9.15 timers — Retrieves the timers (`PVR.GetTimers`)
    [ ] 12.9.16 record — Toggle recording of a channel (`PVR.Record`)
    [ ] 12.9.17 scan — Starts a channel scan (`PVR.Scan`)
    [ ] 12.9.18 toggleTimer — Creates or deletes a onetime timer or timer rule for a given show. If it exists, it will be deleted. If it does not exist, it will be created (`PVR.ToggleTimer`)

  [ ] 12.10 Player
    [ ] 12.10.1 addSubtitle — Add subtitle to the player (`Player.AddSubtitle`)
    [ ] 12.10.2 activePlayers — Returns all active players (`Player.GetActivePlayers`)
    [ ] 12.10.3 item — Retrieves the currently played item (`Player.GetItem`)
    [ ] 12.10.4 players — Get a list of available players (`Player.GetPlayers`)
    [ ] 12.10.5 properties — Retrieves the values of the given properties (`Player.GetProperties`)
    [ ] 12.10.6 viewMode — Get view mode of video player (`Player.GetViewMode`)
    [ ] 12.10.7 goTo — Go to previous/next/specific item in the playlist (`Player.GoTo`)
    [ ] 12.10.8 move — If picture is zoomed move viewport left/right/up/down otherwise skip previous/next (`Player.Move`)
    [x] 12.10.9 open — Start playback of either the playlist with the given ID, a slideshow with the pictures from the given directory or a single file or an item from the database. (`Player.Open`)
        The play-a-file call. `item` is a union; the file form `{"file": <path>}`
        plays an arbitrary path with no library entry required — this is the
        find→play target for the `file` paths surfaced by 12.16.21 / 12.16.12 /
        12.3.6. (Other `item` forms: `{"episodeid"|"movieid"|"songid"|"albumid"|
        "artistid": N}` for library items, `{"playlistid": N}`, `{"directory": …}`.)
        Picks the right player (audio/video) from the file type automatically.
        Request:
        ```json
        {
          "jsonrpc": "2.0",
          "id": 1,
          "method": "Player.Open",
          "params": {
            "item": {
              "file": "/media/Music/ABBA - Discography 1973-2006 Mp3 320 kbps/2005 The Complete Studio Recordings/1973 Ring Ring @320/01 Ring Ring.mp3"
            }
          }
        }
        ```
        Reply (just an ack — playback state is fetched separately):
        ```json
        { "id": 1, "jsonrpc": "2.0", "result": "OK" }
        ```
        Verify with `Player.GetActivePlayers` (→ `playerid` 0, `type` "audio")
        then `Player.GetItem` (→ the now-playing item). Notable: when the file
        matches a scanned library item, Kodi enriches it automatically — opening
        the path above came back as `{"id": 86, "type": "song", "title": "Ring
        Ring", "album": "Ring Ring", "artist": ["Abba"]}` (`id` 86 = the `songid`
        from 12.3.6 step 3), so a file-based open still yields library metadata.
        Video file — identical call with a video path (Earth 2 S01E02 from the
        12.16.21 follow-up). The reply is again `"OK"`, but the player differs:
        `GetActivePlayers` → `playerid` **1**, `type` "video" (audio was `playerid`
        **0**) — so the file extension selects the player. `GetItem` enriched it to
        the library episode: `{"id": 4838, "type": "episode", "title": "First
        Contact (2)", "showtitle": "Earth 2", "season": 1, "episode": 2}`
        (`id` 4838 = the `episodeid` from 12.16.21). Lesson for the generic tool:
        the `playerid` for follow-up control (pause/seek/stop) isn't fixed — read
        it back from `GetActivePlayers` after opening.
        Notes: `result` is just `"OK"`, not the item — confirm via GetItem/noop.
        Opening replaces current playback (it interrupted a paused video here).
        `options` (2nd param) can set `resume`, `playername`, etc.
    [ ] 12.10.10 playPause — Pauses or unpause playback and returns the new state (`Player.PlayPause`)
    [ ] 12.10.11 rotate — Rotates current picture (`Player.Rotate`)
    [ ] 12.10.12 seek — Seek through the playing item (`Player.Seek`)
    [ ] 12.10.13 audioStream — Set the audio stream played by the player (`Player.SetAudioStream`)
    [ ] 12.10.14 partymode — Turn partymode on or off (`Player.SetPartymode`)
    [ ] 12.10.15 repeat — Set the repeat mode of the player (`Player.SetRepeat`)
    [ ] 12.10.16 shuffle — Shuffle/Unshuffle items in the player (`Player.SetShuffle`)
    [ ] 12.10.17 speed — Set the speed of the current playback (`Player.SetSpeed`)
    [ ] 12.10.18 subtitle — Set the subtitle displayed by the player (`Player.SetSubtitle`)
    [ ] 12.10.19 videoStream — Set the video stream played by the player (`Player.SetVideoStream`)
    [ ] 12.10.20 setViewMode — Set view mode of video player (`Player.SetViewMode`)
    [ ] 12.10.21 stop — Stops playback (`Player.Stop`)
    [ ] 12.10.22 zoom — Zoom current picture (`Player.Zoom`)

  [ ] 12.11 Playlist
    [ ] 12.11.1 add — Add item(s) to playlist (`Playlist.Add`)
    [ ] 12.11.2 clear — Clear playlist (`Playlist.Clear`)
    [ ] 12.11.3 items — Get all items from playlist (`Playlist.GetItems`)
    [ ] 12.11.4 playlists — Returns all existing playlists (`Playlist.GetPlaylists`)
    [ ] 12.11.5 properties — Retrieves the values of the given properties (`Playlist.GetProperties`)
    [ ] 12.11.6 insert — Insert item(s) into playlist. Does not work for picture playlists (aka slideshows). (`Playlist.Insert`)
    [ ] 12.11.7 remove — Remove item from playlist. Does not work for picture playlists (aka slideshows). (`Playlist.Remove`)
    [ ] 12.11.8 swap — Swap items in the playlist. Does not work for picture playlists (aka slideshows). (`Playlist.Swap`)

  [ ] 12.12 Profiles
    [ ] 12.12.1 currentProfile — Retrieve the current profile (`Profiles.GetCurrentProfile`)
    [ ] 12.12.2 profiles — Retrieve all profiles (`Profiles.GetProfiles`)
    [ ] 12.12.3 loadProfile — Load the specified profile (`Profiles.LoadProfile`)

  [ ] 12.13 Settings
    [ ] 12.13.1 categories — Retrieves all setting categories (`Settings.GetCategories`)
    [ ] 12.13.2 sections — Retrieves all setting sections (`Settings.GetSections`)
    [ ] 12.13.3 settingValue — Retrieves the value of a setting (`Settings.GetSettingValue`)
    [ ] 12.13.4 settings — Retrieves all settings (`Settings.GetSettings`)
    [ ] 12.13.5 resetSettingValue — Resets the value of a setting (`Settings.ResetSettingValue`)
    [ ] 12.13.6 setSettingValue — Changes the value of a setting (`Settings.SetSettingValue`)

  [ ] 12.14 System
    [ ] 12.14.1 ejectOpticalDrive — Ejects or closes the optical disc drive (if available) (`System.EjectOpticalDrive`)
    [ ] 12.14.2 properties — Retrieves the values of the given properties (`System.GetProperties`)
    [ ] 12.14.3 hibernate — Puts the system running Kodi into hibernate mode (`System.Hibernate`)
    [ ] 12.14.4 reboot — Reboots the system running Kodi (`System.Reboot`)
    [ ] 12.14.5 shutdown — Shuts the system running Kodi down (`System.Shutdown`)
    [ ] 12.14.6 suspend — Suspends the system running Kodi (`System.Suspend`)

  [ ] 12.15 Textures
    [ ] 12.15.1 textures — Retrieve all textures (`Textures.GetTextures`)
    [ ] 12.15.2 removeTexture — Remove the specified texture (`Textures.RemoveTexture`)

  [ ] 12.16 VideoLibrary
    [ ] 12.16.1 clean — Cleans the video library for non-existent items (`VideoLibrary.Clean`)
    [ ] 12.16.2 export — Exports all items from the video library (`VideoLibrary.Export`)
    [ ] 12.16.3 availableArt — Retrieve all potential art URLs for a media item by art type (`VideoLibrary.GetAvailableArt`)
    [ ] 12.16.4 availableArtTypes — Retrieve a list of potential art types for a media item (`VideoLibrary.GetAvailableArtTypes`)
    [ ] 12.16.5 episodeDetails — Retrieve details about a specific tv show episode (`VideoLibrary.GetEpisodeDetails`)
    [ ] 12.16.6 episodes — Retrieve all tv show episodes (`VideoLibrary.GetEpisodes`)
    [ ] 12.16.7 genres — Retrieve all genres (`VideoLibrary.GetGenres`)
    [ ] 12.16.8 inProgressTVShows — Retrieve all in progress tvshows (`VideoLibrary.GetInProgressTVShows`)
    [ ] 12.16.9 movieDetails — Retrieve details about a specific movie (`VideoLibrary.GetMovieDetails`)
    [ ] 12.16.10 movieSetDetails — Retrieve details about a specific movie set (`VideoLibrary.GetMovieSetDetails`)
    [ ] 12.16.11 movieSets — Retrieve all movie sets (`VideoLibrary.GetMovieSets`)
    [x] 12.16.12 movies — Retrieve all movies (`VideoLibrary.GetMovies`)
        Same find pattern as 12.16.21 (GetTVShows): `filter` with
        `{field:"title", operator:"contains", value:…}` (case-insensitive), plus
        `sort`, `limits`, `properties`. Unlike a show, a movie has a direct
        playable `file` — handy for "find then play".
        Request:
        ```json
        {
          "jsonrpc": "2.0",
          "id": 1,
          "method": "VideoLibrary.GetMovies",
          "params": {
            "filter": {
              "field": "title",
              "operator": "contains",
              "value": "alien"
            },
            "properties": ["title", "year", "genre", "rating", "playcount", "runtime", "file"],
            "sort": { "method": "year", "order": "ascending" }
          }
        }
        ```
        Reply (Kodi 20.5; library still mid-scan, so only one match so far —
        the query returns all matches once scraped). Movie shape differs from a
        show: `movieid` handle, `file` is the playable path, `genre` is an array,
        `rating` a float, `runtime` in seconds, `playcount` (no episode counts):
        ```json
        {
          "id": 1,
          "jsonrpc": "2.0",
          "result": {
            "limits": { "start": 0, "end": 1, "total": 1 },
            "movies": [
              {
                "movieid": 1,
                "title": "Alien",
                "label": "Alien",
                "year": 1979,
                "genre": ["Horror", "Science Fiction"],
                "rating": 8.170000076293945,
                "runtime": 7020,
                "playcount": 0,
                "file": "/media/Movies/Alien/Alien.1979.Directors.Cut.720p.BluRay.x264.AAC-ETRG.mp4"
              }
            ]
          }
        }
        ```
        Notes: no filter → all movies. Empty `movies:[]`/`total:0` when nothing
        matches (or not yet scraped). Field name is `title` (not `name`).
    [ ] 12.16.13 musicVideoDetails — Retrieve details about a specific music video (`VideoLibrary.GetMusicVideoDetails`)
    [ ] 12.16.14 musicVideos — Retrieve all music videos (`VideoLibrary.GetMusicVideos`)
    [ ] 12.16.15 recentlyAddedEpisodes — Retrieve all recently added tv episodes (`VideoLibrary.GetRecentlyAddedEpisodes`)
    [ ] 12.16.16 recentlyAddedMovies — Retrieve all recently added movies (`VideoLibrary.GetRecentlyAddedMovies`)
    [ ] 12.16.17 recentlyAddedMusicVideos — Retrieve all recently added music videos (`VideoLibrary.GetRecentlyAddedMusicVideos`)
    [ ] 12.16.18 seasonDetails — Retrieve details about a specific tv show season (`VideoLibrary.GetSeasonDetails`)
    [ ] 12.16.19 seasons — Retrieve all tv seasons (`VideoLibrary.GetSeasons`)
    [ ] 12.16.20 tVShowDetails — Retrieve details about a specific tv show (`VideoLibrary.GetTVShowDetails`)
    [x] 12.16.21 tVShows — Retrieve all tv shows (`VideoLibrary.GetTVShows`)
        Find shows by name: `filter` with `{field:"title", operator:"contains", value:…}`
        (case-insensitive). Also supports `sort`, `limits`, and `properties`.
        Request:
        ```json
        {
          "jsonrpc": "2.0",
          "id": 1,
          "method": "VideoLibrary.GetTVShows",
          "params": {
            "filter": {
              "field": "title",
              "operator": "contains",
              "value": "earth"
            },
            "properties": ["title", "year", "episode", "watchedepisodes"],
            "sort": { "method": "title", "order": "ascending" }
          }
        }
        ```
        Reply (Kodi 20.5; `tvshowid` is the handle for follow-up calls; `episode`/
        `watchedepisodes` give progress; `label`==`title`):
        ```json
        {
          "id": 1,
          "jsonrpc": "2.0",
          "result": {
            "limits": { "start": 0, "end": 4, "total": 4 },
            "tvshows": [
              { "tvshowid": 94, "title": "Earth 2",                "label": "Earth 2",                "year": 1994, "episode": 21, "watchedepisodes": 0 },
              { "tvshowid": 54, "title": "Earth: Final Conflict",   "label": "Earth: Final Conflict",   "year": 1997, "episode": 6,  "watchedepisodes": 0 },
              { "tvshowid": 46, "title": "Elliott from Earth",      "label": "Elliott from Earth",      "year": 2021, "episode": 16, "watchedepisodes": 0 },
              { "tvshowid": 35, "title": "Search for Second Earth", "label": "Search for Second Earth", "year": 2018, "episode": 4,  "watchedepisodes": 0 }
            ]
          }
        }
        ```
        Notes: no filter → all shows. Empty `tvshows:[]` with `total:0` when nothing
        matches (or not yet scraped). Field name is `title` (not `name`).

        Follow-up — request one specific episode of a found show. Take the
        `tvshowid` from the reply above (Earth 2 = 94) and call
        `VideoLibrary.GetEpisodes` (see 12.16.6) with `season` + an episode-number
        `filter` to pin a single S/E. The `file` it returns is the playable path,
        and `episodeid` is the handle for `Player.Open {episodeid}` /
        `GetEpisodeDetails`.
        Request (Earth 2, S01E02 — note `episode` filter value is a string):
        ```json
        {
          "jsonrpc": "2.0",
          "id": 2,
          "method": "VideoLibrary.GetEpisodes",
          "params": {
            "tvshowid": 94,
            "season": 1,
            "filter": { "field": "episode", "operator": "is", "value": "2" },
            "properties": ["title", "season", "episode", "file", "firstaired", "runtime"]
          }
        }
        ```
        Reply (exactly one episode):
        ```json
        {
          "id": 2,
          "jsonrpc": "2.0",
          "result": {
            "limits": { "start": 0, "end": 1, "total": 1 },
            "episodes": [
              {
                "episodeid": 4838,
                "title": "First Contact (2)",
                "label": "1x02. First Contact (2)",
                "season": 1,
                "episode": 2,
                "firstaired": "1994-11-06",
                "runtime": 0,
                "file": "/media/Serials/Earth 2/Earth.2.s01.e02.The.man.who.fell.to.earth.(Two).avi"
              }
            ]
          }
        }
        ```
        Notes: omit the `episode` filter to list a whole season (add `limits` for
        paging; `total` is the season's episode count). `runtime` is 0 when the
        scraper didn't supply a duration. `label` reflects the scraped title, which
        can differ from the on-disk filename.
    [ ] 12.16.22 tags — Retrieve all tags (`VideoLibrary.GetTags`)
    [ ] 12.16.23 refreshEpisode — Refresh the given episode in the library (`VideoLibrary.RefreshEpisode`)
    [ ] 12.16.24 refreshMovie — Refresh the given movie in the library (`VideoLibrary.RefreshMovie`)
    [ ] 12.16.25 refreshMusicVideo — Refresh the given music video in the library (`VideoLibrary.RefreshMusicVideo`)
    [ ] 12.16.26 refreshTVShow — Refresh the given tv show in the library (`VideoLibrary.RefreshTVShow`)
    [ ] 12.16.27 removeEpisode — Removes the given episode from the library (`VideoLibrary.RemoveEpisode`)
    [ ] 12.16.28 removeMovie — Removes the given movie from the library (`VideoLibrary.RemoveMovie`)
    [ ] 12.16.29 removeMusicVideo — Removes the given music video from the library (`VideoLibrary.RemoveMusicVideo`)
    [ ] 12.16.30 removeTVShow — Removes the given tv show from the library (`VideoLibrary.RemoveTVShow`)
    [ ] 12.16.31 scan — Scans the video sources for new library items (`VideoLibrary.Scan`)
    [ ] 12.16.32 setEpisodeDetails — Update the given episode with the given details (`VideoLibrary.SetEpisodeDetails`)
    [ ] 12.16.33 setMovieDetails — Update the given movie with the given details (`VideoLibrary.SetMovieDetails`)
    [ ] 12.16.34 setMovieSetDetails — Update the given movie set with the given details (`VideoLibrary.SetMovieSetDetails`)
    [ ] 12.16.35 setMusicVideoDetails — Update the given music video with the given details (`VideoLibrary.SetMusicVideoDetails`)
    [ ] 12.16.36 setSeasonDetails — Update the given season with the given details (`VideoLibrary.SetSeasonDetails`)
    [ ] 12.16.37 setTVShowDetails — Update the given tvshow with the given details (`VideoLibrary.SetTVShowDetails`)

  [ ] 12.17 XBMC
    [ ] 12.17.1 infoBooleans — Retrieve info booleans about Kodi and the system (`XBMC.GetInfoBooleans`)
    [ ] 12.17.2 infoLabels — Retrieve info labels about Kodi and the system (`XBMC.GetInfoLabels`)

---

## 13. Playback history

  A chronological, cross-instance log of what was actually played, so the AI can
  answer "what did I watch last week", "what music did I put on yesterday",
  "what was on in the kids' room last month". Designed here; the read-back tool
  is parked until we have a real file to look at (§13.10).

  [x] 13.1 **Purpose, and how it differs from state (§8).** The per-instance
  state file (§8) is *forward-looking intent* — `last_played`, last episode per
  show — to drive "what next", resume and handoff. This history is the opposite:
  a *backward-looking, append-mostly log* of what already played, spanning **all
  instances in one list**. Different shape, different file, different lifecycle —
  history and state do **not** share storage (a history append must never race a
  state write, same separation rationale as §8.2 vs §7.1).

  [x] 13.2 **No monitoring — capture is call-driven only.** We do not poll and we
  do not subscribe to Kodi's WebSocket push (§2.1, §10.1). The only moments we
  learn what is playing are the moments a tool call runs and produces a
  now-playing snapshot. Consequences, stated plainly so the blind spots are on
  record rather than discovered later:
    [x] 13.2.1 Playback the user starts from the **physical remote / Kodi UI** is
    invisible — we never see it unless some later tool call happens to snapshot it.
    [x] 13.2.2 **No playlist/queue tracking.** We record the item playing at the
    moment of the call (the *first* item when a play starts); subsequent tracks
    Kodi advances to on its own are not seen. (No `Playlist`/queue support yet,
    §12.11 — deliberately out of scope for v1.)
    [x] 13.2.3 The log is therefore a record of *what the assistant caused or
    observed*, not a complete audit of the box. Honest about its gaps; good
    enough for "what did we play".

  [x] 13.3 **The snapshot is the raw material — no extra Kodi call.** Every
  playback-affecting tool already ends by building the canonical `player_state()`
  snapshot (§5.4: `state`, `type`, `file`, `label`, `title`, `time`,
  `totaltime`) — `play`/`playfile`/`random`/`handoff` and the transport Buttons.
  History **reuses that exact snapshot**; from it plus the instance key and a
  capture timestamp we compose one entry (point 4 of the brief). No round-trip
  beyond the one the tool already makes.
    [x] 13.3.1 **The escape hatch too.** The `rpc` tool (§11.6.6) returns Kodi's
    raw `result` verbatim and takes **no** snapshot — so an `rpc` that started
    playback (a hand-rolled `Player.Open`) would slip past history. After a
    *successful* `rpc` call, take a `player_state()` snapshot **purely to feed
    history**. This is a side effect: it must **not** change `rpc`'s verbatim
    return value (§11.6.6 still returns the raw `result` unshaped). A
    stopped/empty snapshot records nothing (§13.5), so non-playback `rpc` calls
    cost one cheap extra `Player.GetActivePlayers` and append nothing.

  [ ] 13.4 **Entry shape (draft).** One object per played item, built straight
  from the snapshot (§13.3) plus the instance key/name and capture time:
  ```json
  // a TV episode
  {
    "at": "<iso8601>",
    "instance": "hall",
    "name": "Living Room TV",
    "kind": "video",
    "media": "episode",
    "id": 4838,
    "title": "First Contact (2)",
    "showtitle": "Earth 2",
    "season": 1,
    "episode": 2,
    "file": "/media/Serials/Earth 2/Earth.2.s01.e02.....avi",
    "label": "1x02. First Contact (2)"
  }
  // a song / album track
  {
    "at": "<iso8601>",
    "instance": "hall",
    "name": "Living Room TV",
    "kind": "audio",
    "media": "song",
    "id": 86,
    "title": "Ring Ring",
    "album": "Ring Ring",
    "artist": ["Abba"],
    "track": 1,
    "file": "/media/Music/.../01 Ring Ring.mp3",
    "label": "Ring Ring"
  }
  ```
  Per-media fields are included when they apply (a movie carries neither
  `showtitle`/`season`/`episode` nor `album`/`artist`); omit what's empty.
  We do **not** store `time`/`totaltime`: at capture a freshly started item sits
  at ≈0, and without polling (§13.2) we never learn whether or how far it
  finished — so the entry asserts only *"this was played at `at`"*. `label`/`file`
  are the human-identifiable fields (for this library the path itself already
  carries artist/album/show).
    [x] 13.4.0 **`at` — the capture timestamp.** Server wall-clock at the moment
    the snapshot was taken and the entry written (≈ when playback started, §13.2.2),
    from `g_date_time_*`. It is the backbone of the feature: every "last week /
    yesterday / last month" query (§13.10) filters on it, it is the sort key, and
    it drives the age-based trim (§13.8.2). Store it **with an explicit timezone**
    — UTC `…Z` or local-with-offset (`2026-06-09T19:30:00+02:00`) — never a bare
    local time: an offset-less stamp makes range queries wrong across DST/zone
    changes. Pick one form (UTC is the safe default; convert to local only when
    presenting). `at` records *start*, not completion or duration (§13.2).
    [x] 13.4.1 **Media type and library id are free — record them in v1.** The
    snapshot's current `type` (from `Player.GetActivePlayers`) is only the
    **player kind** — `audio`/`video` — so it tells music from video but **not**
    movie from TV episode from music video. The precise media type is already in
    the `Player.GetItem` response we make every snapshot: Kodi injects `type`
    (`song`/`episode`/`movie`/`musicvideo`/`picture`/`channel`/`unknown`) and the
    library `id` (`songid`/`episodeid`/`movieid`) automatically — they are
    **identity fields, not requestable `properties`** (absent from
    `List.Item.Base`/`List.Item.All` in the introspect schema; confirmed live in
    §12.10.9: `{"id":4838,"type":"episode"}`, `{"id":86,"type":"song"}`), so we
    get them at **zero extra Kodi cost** — `player_state()` just discards them
    today. So the history entry above carries both: `kind` = player kind
    (audio/video), `media` = the real media type, `id` = the library id. Caveats:
    a `playfile` of a path **not** in the library can't be enriched → `media` is
    `"unknown"` and `id` is `-1`; and surfacing the media type means using a key
    other than `type` (the snapshot already spends `type` on the player kind), so
    this entry renames them `kind` + `media` to avoid the collision.
    [x] 13.4.1.1 **What `id` is good for, and isn't.** The stored `id` is the
    library handle for `Player.Open {<media>id: N}` — so a future read tool
    (§13.10) could replay a logged item by `id` instead of re-searching by name.
    But it is a *convenience* handle, not a durable key: it is **library-scoped**
    (only valid on a box that shares that library backend — the §5.6 shared-library
    assumption; an `id` logged for one instance must **not** be replayed on another
    with a separate local library), and **not stable across a library clean/rescan**
    (ids can be renumbered, so an old `id` may later resolve to a different item or
    none). So treat `file`/`label`/`showtitle`/`album` as the durable identifiers
    and `id` as a best-effort replay shortcut.
    [x] 13.4.2 **Show/episode and album/artist — record them in v1.** For a
    useful log the entry must name *which show* and *which episode* for TV, and
    the *album*/*artist* for music — so v1 records, per media type:
    `showtitle` + `season` + `episode` (episodes), `album` + `artist` (+ `track`)
    (songs). Unlike `type`/`id` (§13.4.1) these are **not** auto-injected — they
    must be named in the `properties` array of `Player.GetItem` — but they are
    all valid `List.Item.Base` properties reachable in the **same single call**
    we already make (it currently asks only `["title","file"]`,
    `src/mk-tools.c`), so widening that list costs **no extra Kodi round-trip**,
    only a longer field list. `artist` is an array of strings (e.g. `["Abba"]`);
    `season`/`episode`/`track` are integers; `showtitle`/`album` are strings.
    [x] 13.4.3 **Open implementation choice — where the wider `GetItem` lives.**
    Two ways to source the §13.4.2 fields; pick one when building `mk-history`:
      [x] 13.4.3.1 *Widen the shared `player_state()` `GetItem`* so the now-playing
      snapshot itself carries show/episode/album/artist. One call, one source of
      truth, history just copies it — and every `status`/transport reply gets
      richer too (arguably a feature: "playing S01E02 of Earth 2"). Cost: a larger
      status JSON on every call. **Recommended.**
      [x] 13.4.3.2 *Keep `player_state()` lean and have the history path issue its
      own richer `Player.GetItem`* when it records. Keeps the shared snapshot
      small, but adds one extra `GetItem` per recorded play (rare, dedup'd — §13.5)
      and a second code path that can drift from the snapshot. **Considered and
      rejected** in favour of §13.4.3.1: the single-source-of-truth snapshot and
      the richer status replies outweigh a marginally larger status JSON, and one
      code path can't drift from itself.
    Decide against a real `history.json` (§13.10) — but the fields themselves are
    committed for v1; this is only *how* to fetch them.
    [x] 13.4.4 **Deeper metadata stays parked.** Anything past §13.4.2 — genre,
    year, rating, full cast, album art — is not worth carrying in a play log;
    leave it to a later `*Details` lookup keyed off the stored `id` if ever
    needed. Same wait-for-real-data discipline as point 7 of the brief.

  [x] 13.5 **When to append — dedup, don't flood.** Appending on every snapshot
  would bury the log: every pause, `volume`, `seek`, `noop` re-observes the same
  item. So:
    [x] 13.5.1 Record only when the snapshot shows something **loaded** —
    `state != "stopped"`; a stopped snapshot appends nothing.
    [x] 13.5.2 Record only when it is a **new item** — compare against the most
    recent entry *for that instance*; same `file` (or id) ⇒ skip (same thing,
    just re-observed); a different `file` ⇒ append.
    This yields one entry per distinct thing played per instance — the "first
    item when play starts" (point 3) — and naturally ignores pause/resume/volume
    churn on the same item.

  [x] 13.6 **The file.** One **global** file (all instances in one list, point
  4), separate from config (§7) and per-instance state (§8):
  `${XDG_STATE_HOME:-~/.local/state}/mcp-kodi/history.json`. A JSON array of
  entries via the json-glib we already use — symmetric with §7/§8. Newest-first
  reads naturally for "recent" (order TBD with the read tool, §13.10). Directory
  `0700` on first write; file `0600` (it records viewing habits — mildly private).

  [x] 13.7 **Concurrent writers — lock, do not "last-write-wins".** Several
  copies of this server can run at once (one per MCP client/session, §2.1), all
  appending to the one file. **Bare "last write wins via atomic rename" is the
  wrong model for an append log:** two processes each read base *N*, each append
  their own entry, each `rename()` — the second clobbers the first's entry. That
  is the classic lost-update, and an append log must not silently drop entries.
  So **serialize the whole read-modify-write** instead:
    [x] 13.7.1 Take an exclusive advisory lock (`flock` `LOCK_EX`) on the history
    file (or a sidecar `history.json.lock`) for the critical section.
    [x] 13.7.2 Under the lock: read the current array → dedup/append the new entry
    (§13.5) → trim (§13.8) → write a temp file in the same dir, `fsync`, then
    `rename()` over the target (atomic replacement — the §7.4/§8.4 discipline).
    [x] 13.7.3 Release the lock. MCP calls are human-paced and a record is a few
    KB, so the lock is held microseconds and contention is negligible.
    [x] 13.7.4 *Alternative considered:* JSONL + `O_APPEND` (kernel-atomic for
    small records, no read needed to append) — but the size-trim (§13.8) still
    needs a locked rewrite, so that is two mechanisms where one locked
    read-modify-write does the job. Prefer the single locked path for simplicity
    and json-glib symmetry; revisit only if append volume ever grows.

  [x] 13.8 **Retention / size cap.** The log must answer "last week / last month"
  comfortably, so keep plenty — but bound it so the file can't grow without
  limit (point 6). On each write, *after* appending, trim the oldest entries
  beyond a cap, oldest-first:
    [x] 13.8.1 By count — keep the newest `MK_HISTORY_MAX` entries (named
    constant; start generous, e.g. 10000).
    [x] 13.8.2 And/or by age — drop entries older than ~180 days.
    Both bounds are constants in `mk-history`, tuned once we see real volume.
    With sparse, call-driven entries (§13.2) 10k rows is many months —
    comfortably past "last month".

  [ ] 13.9 **Source layout.** New module `mk-history.{c,h}` (added to the §6.1
  tree alongside `mk-state`):
    [x] 13.9.1 `mk_history_record(self, instance, name, snapshot)` — the write
    path: lock → read → dedup/append → trim → atomic write (§13.5–13.8). Wired
    from the single point every playback-affecting handler funnels through —
    `player_state()` itself — so transport/play/status/noop all feed it for free
    (dedup, §13.5, absorbs the re-observations); `rpc` issues its own post-call
    `player_state()` snapshot (§13.3.1), after a brief settle delay so a
    hand-rolled `Player.Open` has registered before the snapshot. The `instance`
    key carries the box's display label as `name` (added to the signature).
    [x] 13.9.2 **Best-effort — history never fails the call.** A history write
    failure (lock, disk, parse) must **not** fail the underlying tool call: log
    to stderr (§3.1) and carry on. The user's `play` succeeded even if logging it
    didn't. Realized in `mk_history_record`: every failure path `g_warning`s and
    returns FALSE, it reports no GError, and a parse/shape error on the existing
    file aborts **without** clobbering it.
    [ ] 13.9.3 `mk_history_read(...)` — the read path for the future tool
    (§13.10); signature TBD.

  [ ] 13.10 **Read tool — deferred (point 7).** A future `history` tool to read
  the log back, likely with filters: `instance` (or `"*"`), a time window
  (`since`/`until`, or "last 7 days"), `type`, and a `limit`. Exact args and
  output shaping are deliberately left open until we have a real `history.json`
  to inspect and can see what questions it must actually answer — the same
  wait-for-real-data stance as §11.7 and §13.4.1. For now the server only
  **writes** the log; nothing reads it programmatically yet.
