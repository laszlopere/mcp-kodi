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
  [ ] 1.3 **HTTP:** `libsoup-3.0` as the HTTP client (`SoupSession`,
  `SoupMessage`, `soup_session_send_and_read`).
  [ ] 1.4 **Transport to Kodi:** JSON-RPC POSTed over **HTTPS** to a Caddy
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

  [ ] 4.1 Build requests with `JsonBuilder`:
  `{ "jsonrpc": "2.0", "id": <n>, "method": <m>, "params": <obj> }`.
  [ ] 4.2 Calls are made *against a named instance* (see [§7](#7-configuration-file)):
  resolve the instance to its `scheme`/`host`/`auth`/`insecure`, then POST to
  `<scheme>://<host>/jsonrpc` with `Content-Type: application/json` and HTTP Basic
  auth. A tool that omits the instance uses the configured `default`.
  [ ] 4.3 Reuse one `SoupSession` for the whole process across all instances
  (it can talk to several hosts). For each instance whose config sets `insecure`,
  accept the self-signed cert — connect the session's `accept-certificate` signal
  and accept only for that instance's host, not blanket.
  [ ] 4.4 Synchronous send per tool call (`soup_session_send_and_read`) is fine —
  calls are serialised by the AI and latency is human-scale.
  [ ] 4.5 Parse the response; surface Kodi's `error` member as a tool error.
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
  [ ] 8.2 **Location:** `${XDG_STATE_HOME:-~/.local/state}/mcp-kodi/state.json`.
  [ ] 8.3 **Shape (draft):** state is keyed by instance name, mirroring the config
  ([§7](#7-configuration-file)):
  ```json
  {
    "version": 2,
    "instances": {
      "hall": {
        "last_played": { "type": "episode", "id": 1234, "label": "...", "position": "00:12:34", "at": "<iso8601>" },
        "shows": { "<tvshowid>": { "last_episode_id": 5678, "season": 3, "episode": 7, "at": "<iso8601>" } }
      },
      "bedroom": { "last_played": null, "shows": {} }
    }
  }
  ```
  [ ] 8.4 **Writes:** on `play`/`random`/`handoff` for the target instance (record
  `last_played` under that instance; if it's an episode, update that instance's
  `shows` entry). `handoff` also captures the resumed `position`. Atomic: write a
  temp file in the same dir, `fsync`, then `rename()` over the target.
  [ ] 8.5 **Reads:** available to handlers that want "what next" context, scoped
  to an instance; may later back a `resume` / `next_episode` tool. Keep the file
  optional — absence, or a missing instance key, = empty state for that instance.
  [ ] 8.6 **Back-compat:** a `version: 1` flat state file is migrated into
  `instances.default` on first read.

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
  [ ] 11.4 Kodi JSON-RPC client, per-instance (`mk-kodi`)
  [ ] 11.5 MCP stdio transport + dispatch (`mk-stdio`, `mk-mcp`)
  [ ] 11.6 Tool table + handlers, incl. `instance` arg, `seek`, `handoff` (`mk-tools`)
  [ ] 11.7 Playback state file, per-instance (`mk-state`)
  [ ] 11.8 Build clean, test against live Kodi, write README
