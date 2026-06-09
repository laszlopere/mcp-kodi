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
  volume, mute) plus a generic `rpc` escape hatch for anything not modelled.
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
  | `instances` | —                                      | none — list configured instance names + which is `default` |
  | `ping`      | `instance?`                            | `JSONRPC.Ping` |
  | `info`      | `instance?`                            | `Application.GetProperties` (name, version, volume, muted) |
  | `notify`    | `instance?`, `title`, `message`, `displaytime?` | `GUI.ShowNotification` |
  | `players`   | `instance?`                            | `Player.GetActivePlayers` |
  | `playpause` | `instance?`                            | `Player.PlayPause` (on active player) |
  | `stop`      | `instance?`                            | `Player.Stop` (on active player) |
  | `play`      | `instance?`, `type` (song/album/artist/movie/episode/musicvideo), `id` | `Player.Open` with the matching `item` key |
  | `seek`      | `instance?`, `time` (h/m/s) **or** `percentage` | `Player.Seek` on the active player |
  | `handoff`   | `from`, `to`                           | capture active item + position on `from` → `Player.Stop` on `from` → `Player.Open` on `to` → `Player.Seek` to that position |
  | `search`    | `type` (tvshow/movie/album/artist/song), `query` | `VideoLibrary.*` / `AudioLibrary.*` Get + filter |
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
  | `playfile`  | `instance?`, `path`                    | `Player.Open` with a `file` item — play a path not in the library |
  | `files`     | `instance?`, `path?` (a source/dir; omitted → sources) | `Files.GetSources` / `Files.GetDirectory` |
  | `rpc`       | `instance?`, `method`, `params?` (object) | passthrough — any JSON-RPC method |

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
      └── mk-state.{c,h}    playback state file (json, atomic write)
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
      "hall":    { "host": "hall.example.local:8443",    "auth": "kodi:<password>", "scheme": "https", "insecure": true },
      "bedroom": { "host": "bedroom.example.local:8443", "auth": "kodi:<password>", "scheme": "https", "insecure": true },
      "kids":    { "host": "kids.example.local:8443",    "auth": "kodi:<password>", "scheme": "https", "insecure": true }
    }
  }
  ```
  (`auth` is `user:pass` for HTTP Basic; `insecure: true` accepts the self-signed
  cert — the JSON equivalent of curl `-k`. Instance names are free-form user
  labels; tools reference them, and `default` is used when a tool omits
  `instance`.)
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
  [ ] 7.5 **Save trigger (TBD):** the save primitive lives in `mk-config`
  regardless of caller; the entry point (a first-run/setup path, a `--save` flag,
  or a future `configure` tool) is to be decided.
  [x] 7.6 **Back-compat:** a `version: 1` flat file (single `host`/`auth`/… at the
  top level) is read as one instance named `default`; on next save it is rewritten
  in the `version: 2` instances shape.

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
    [ ] 11.6.2 Knobs — set to a scalar value; $value = the knob argument:
      [ ] 11.6.2.1 volume
        `{"method":"Application.SetVolume","params":{"volume":"$value"}}`
  [ ] 11.7 Resources/prompts
  [ ] 11.8 Playback state file, per-instance (`mk-state`)
  [ ] 11.9 Build clean, test against live Kodi, write README

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
    [ ] 12.3.4 albums — Retrieve all albums from specified artist (and role) or that has songs of the specified genre (`AudioLibrary.GetAlbums`)
    [ ] 12.3.5 artistDetails — Retrieve details about a specific artist (`AudioLibrary.GetArtistDetails`)
    [ ] 12.3.6 artists — Retrieve all artists. For backward compatibility by default this implicitly does not include those that only contribute other roles, however absolutely all artists can be returned using allroles=true (`AudioLibrary.GetArtists`)
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
    [ ] 12.3.17 songs — Retrieve all songs from specified album, artist or genre (`AudioLibrary.GetSongs`)
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
    [ ] 12.10.9 open — Start playback of either the playlist with the given ID, a slideshow with the pictures from the given directory or a single file or an item from the database. (`Player.Open`)
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
    [ ] 12.16.12 movies — Retrieve all movies (`VideoLibrary.GetMovies`)
    [ ] 12.16.13 musicVideoDetails — Retrieve details about a specific music video (`VideoLibrary.GetMusicVideoDetails`)
    [ ] 12.16.14 musicVideos — Retrieve all music videos (`VideoLibrary.GetMusicVideos`)
    [ ] 12.16.15 recentlyAddedEpisodes — Retrieve all recently added tv episodes (`VideoLibrary.GetRecentlyAddedEpisodes`)
    [ ] 12.16.16 recentlyAddedMovies — Retrieve all recently added movies (`VideoLibrary.GetRecentlyAddedMovies`)
    [ ] 12.16.17 recentlyAddedMusicVideos — Retrieve all recently added music videos (`VideoLibrary.GetRecentlyAddedMusicVideos`)
    [ ] 12.16.18 seasonDetails — Retrieve details about a specific tv show season (`VideoLibrary.GetSeasonDetails`)
    [ ] 12.16.19 seasons — Retrieve all tv seasons (`VideoLibrary.GetSeasons`)
    [ ] 12.16.20 tVShowDetails — Retrieve details about a specific tv show (`VideoLibrary.GetTVShowDetails`)
    [ ] 12.16.21 tVShows — Retrieve all tv shows (`VideoLibrary.GetTVShows`)
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
