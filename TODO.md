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

  [ ] 1.1 **Stack:** GLib + GObject + GIO, built with **autotools**
  (`configure.ac` + `PKG_CHECK_MODULES`).
  [ ] 1.2 **JSON:** `json-glib-1.0` for all parsing and serialisation
  (`JsonParser`, `JsonBuilder`, `JsonNode`, `JsonObject`, `JsonGenerator`).
  [ ] 1.3 **HTTP:** `libsoup-3.0` as the HTTP client (`SoupSession`,
  `SoupMessage`, `soup_session_send_and_read`).
  [ ] 1.4 **Transport to Kodi:** JSON-RPC POSTed over **HTTPS** to a Caddy
  reverse proxy in front of Kodi. The proxy uses `tls internal` (a self-signed
  cert), so the client must **accept the untrusted certificate** explicitly
  (equivalent to curl `-k`).
  [ ] 1.5 **Config from a file + environment, never hardcoded:** read the Kodi
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
  [ ] 4.2 POST to `${KODI_SCHEME}://${KODI_HOST}/jsonrpc` with
  `Content-Type: application/json` and HTTP Basic auth from `KODI_AUTH`.
  [ ] 4.3 Reuse one `SoupSession` for the process lifetime. If config indicates
  a self-signed cert (`-k`), connect the session's `accept-certificate` signal
  to unconditionally accept (only for the configured host).
  [ ] 4.4 Synchronous send per tool call (`soup_session_send_and_read`) is fine —
  calls are serialised by the AI and latency is human-scale.
  [ ] 4.5 Parse the response; surface Kodi's `error` member as a tool error.
  Return the `result` member (or a small shaped summary) as the tool's JSON text.

  [ ] 4.6 **Server-side setup:** how to enable Kodi's JSON-RPC web server and
  front it with the Caddy HTTPS reverse proxy is documented for end users in
  [docs/kodi-server-setup.md](docs/kodi-server-setup.md).

---

## 5. Tool surface

  [ ] 5.1 Each tool has a JSON Schema `inputSchema`. Names and arguments:

  | Tool        | Args                                   | Kodi method(s) |
  |-------------|----------------------------------------|----------------|
  | `ping`      | —                                      | `JSONRPC.Ping` |
  | `info`      | —                                      | `Application.GetProperties` (name, version, volume, muted) |
  | `notify`    | `title`, `message`, `displaytime?`     | `GUI.ShowNotification` |
  | `players`   | —                                      | `Player.GetActivePlayers` |
  | `playpause` | —                                      | `Player.PlayPause` (on active player) |
  | `stop`      | —                                      | `Player.Stop` (on active player) |
  | `play`      | `type` (song/album/artist/movie/episode/musicvideo), `id` | `Player.Open` with the matching `item` key |
  | `search`    | `type` (tvshow/movie/album/artist/song), `query` | `VideoLibrary.*` / `AudioLibrary.*` Get + filter |
  | `random`    | `type` (episode/movie/song), `query?`  | resolve filter → pick → `Player.Open` |
  | `episodes`  | `show`, `filter?` (SxxExx or text)     | `VideoLibrary.GetEpisodes` |
  | `status`    | —                                      | `Player.GetActivePlayers` + `Player.GetItem` + `Player.GetProperties` |
  | `volume`    | `level` (0–100)                        | `Application.SetVolume` |
  | `mute`      | `state` (on/off/toggle)                | `Application.SetMute` |
  | `rpc`       | `method`, `params?` (object)           | passthrough — any JSON-RPC method |

  [ ] 5.2 `random` and `search` resolve library ids internally (e.g. a show name
  → `tvshowid` → episodes) so the AI can act by name, not just numeric id.

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

  [ ] 7.1 **Location:** `${XDG_CONFIG_HOME:-~/.config}/mcp-kodi/config.json` — the
  server's own file. No `cli`/shell config in this project; JSON so it round-trips
  through the json-glib we already use and stays symmetric with the state file
  ([§8](#8-playback-state-file)).
  [ ] 7.2 **Shape (draft):**
  ```json
  {
    "version": 1,
    "host": "kodi.example.local:8443",
    "auth": "kodi:<password>",
    "scheme": "https",
    "insecure": true
  }
  ```
  (`auth` is `user:pass` for HTTP Basic; `insecure: true` accepts the self-signed
  cert — the JSON equivalent of curl `-k`.)
  [ ] 7.3 **Load:** on startup read the file if present, then overlay environment
  overrides (`KODI_HOST`, `KODI_AUTH`, `KODI_SCHEME`; `-k` in `KODI_CURL_OPTS`
  sets `insecure`). No file and no env → a clear error telling the user to
  configure. Never hardcode host or credentials.
  [ ] 7.4 **Save:** write the effective config back atomically — temp file in the
  same dir, `fsync`, then `rename()` over the target (same discipline as the state
  file, §8.4). Create the directory `0700` and the file `0600`; it holds a
  password.
  [ ] 7.5 **Save trigger (TBD):** the save primitive lives in `mk-config`
  regardless of caller; the entry point (a first-run/setup path, a `--save` flag,
  or a future `configure` tool) is to be decided.

---

## 8. Playback state file

  [ ] 8.1 **Purpose:** remember, across stateless calls, *what we started to
  play* and *the last episode watched per TV show*, so the AI can resume /
  continue.
  [ ] 8.2 **Location:** `${XDG_STATE_HOME:-~/.local/state}/mcp-kodi/state.json`.
  [ ] 8.3 **Shape (draft):**
  ```json
  {
    "version": 1,
    "last_played": { "type": "episode", "id": 1234, "label": "...", "at": "<iso8601>" },
    "shows": { "<tvshowid>": { "last_episode_id": 5678, "season": 3, "episode": 7, "at": "<iso8601>" } }
  }
  ```
  [ ] 8.4 **Writes:** on `play`/`random` (record `last_played`; if it's an
  episode, update that show's `shows` entry). Atomic: write a temp file in the
  same dir, `fsync`, then `rename()` over the target.
  [ ] 8.5 **Reads:** available to handlers that want "what next" context; may
  later back a `resume` / `next_episode` tool. Keep the file optional — absence =
  empty state.

---

## 9. Build/runtime requirements

  [ ] 9.1 Deps (all present on the dev box): `glib-2.0 >= 2.80`, `gobject-2.0`,
  `gio-2.0`, `json-glib-1.0 >= 1.8`, `libsoup-3.0 >= 3.4`.
  [ ] 9.2 Toolchain: gcc 13, autotools, `pkg-config`.
  [ ] 9.3 Warnings-as-errors during dev (`-Wall -Wextra`); check LSP diagnostics
  after each edit.

---

## 10. Status / next steps

  [x] 10.1 Spec (this file)
  [x] 10.2 Autotools scaffold (configure.ac, Makefile.am, autogen.sh, src
  skeleton)
  [ ] 10.3 Config load + save (`mk-config`)
  [ ] 10.4 Kodi JSON-RPC client (`mk-kodi`)
  [ ] 10.5 MCP stdio transport + dispatch (`mk-stdio`, `mk-mcp`)
  [ ] 10.6 Tool table + handlers (`mk-tools`)
  [ ] 10.7 Playback state file (`mk-state`)
  [ ] 10.8 Build clean, test against live Kodi, write README
