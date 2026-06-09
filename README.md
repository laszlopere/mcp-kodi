# mcp-kodi

An [MCP](https://modelcontextprotocol.io) server that lets an AI assistant
control a [Kodi](https://kodi.tv) media player over Kodi's JSON-RPC API. Written
in C on the GLib stack.

There is **no user-facing CLI**: you talk to the AI, the AI calls this server's
tools, and the server speaks JSON-RPC to Kodi. The only interface is MCP over
stdio.

> **Status — early but already useful (v0.1.0).** The set of dedicated,
> purpose-built tools is deliberately small. What makes the current build
> capable well beyond that list is the **`rpc` escape hatch** (see below): an
> opt-in passthrough to *any* Kodi JSON-RPC method. Between the handful of
> first-class tools and that escape hatch, an assistant can already drive nearly
> everything Kodi exposes.

---

## What it can do

Each tool targets a configured Kodi box by key (`instance`); omit it and the
configured `default` box is used.

| Tool        | What it does |
|-------------|--------------|
| `play`      | Press **Play** on the remote — resume or begin playback on the target box. |
| `pause`     | Press **Pause** — pause the active player. |
| `stop`      | Press **Stop** — stop the active player. |
| `mute`      | Mute the box's audio output. |
| `unmute`    | Unmute the box's audio output. |
| `noop`      | Report the player state without changing anything — a reachability + state probe. |
| `search`    | Find playable **leaf files** by name across `music` / `tv-show` / `movie`, with paging (`limit`/`offset`) and a total count. Resolves the library by name, so the assistant acts by title, not numeric id. |
| `playfile`  | Play one file by path (typically a `search` result's `file`). Kodi auto-selects the audio/video player. Works for any reachable path, in-library or not. |
| `instances` | Read or modify the server's own instance config (`get`/`set`/`remove`). Makes no Kodi call; never returns stored passwords. |
| `rpc`       | **Escape hatch** — send a raw JSON-RPC method to Kodi and return its reply unchanged. Off by default; opt-in per instance (see below). |

Transport tools (`play`/`pause`/`stop`) and `noop` return a small player-state
snapshot — `{ "state": "playing"|"paused"|"stopped", "file", "label", "title",
"time", "totaltime" }` — so the assistant always sees the effect of its action.
`mute`/`unmute` return `{ "muted", "volume" }`.

### The `rpc` escape hatch

`rpc` POSTs any JSON-RPC method you name to the target box and hands back Kodi's
raw `result`, unshaped:

```jsonc
// e.g. raise the GUI volume, activate a window, send an input action…
rpc { "instance": "hall", "method": "GUI.ActivateWindow", "params": { "window": "home" } }
```

This is powerful and unconstrained, so it is **disabled by default** and gated
**per instance**. A box permits `rpc` only when its config object carries
`"allow_rpc": true`. That flag is granted **only by hand-editing `config.json`** —
it is intentionally *not* one of the fields the `instances` tool can write, so
the assistant can never enable its own escape hatch. Opting a box in is an
explicit, out-of-band human decision; calling `rpc` on a box that hasn't opted in
returns a clean error and makes no Kodi request.

See the full Kodi method surface in
[docs/kodi-jsonrpc-catalog.md](docs/kodi-jsonrpc-catalog.md).

---

## Requirements

Build-time (all via `pkg-config`):

- A C compiler and the autotools (`autoconf`, `automake`, `libtool`, `pkg-config`)
- `glib-2.0` ≥ 2.80, `gobject-2.0`, `gio-2.0`, `gio-unix-2.0`
- `json-glib-1.0` ≥ 1.6
- `libsoup-3.0` ≥ 3.0

On Debian/Ubuntu:

```sh
sudo apt install build-essential autoconf automake libtool pkg-config \
  libglib2.0-dev libjson-glib-dev libsoup-3.0-dev
```

Run-time: a reachable Kodi box with its JSON-RPC web server enabled — see
[Kodi server setup](#kodi-server-setup).

---

## Build

```sh
./autogen.sh        # first checkout only — generates ./configure
./configure
make
```

The binary lands at `src/mcp-kodi`. `make install` is supported but not required;
you can register the in-tree binary directly with your MCP client.

---

## Configuration

The server reads `${XDG_CONFIG_HOME:-~/.config}/mcp-kodi/config.json`. It holds a
map of named Kodi instances and names one `default`:

```json
{
  "version": 2,
  "default": "hall",
  "instances": {
    "hall":    { "name": "Living Room TV", "host": "hall.example.local:8443", "auth": "kodi:<password>", "scheme": "https", "insecure": true, "allow_rpc": true },
    "bedroom": { "name": "Bedroom",        "host": "bedroom.example.local:8443", "auth": "kodi:<password>", "scheme": "https", "insecure": true }
  }
}
```

| Field       | Meaning |
|-------------|---------|
| instance *key* | The short id every tool references (`hall`, `bedroom`, …). |
| `default`   | The key used when a tool omits `instance`. |
| `name`      | Optional human-readable label, surfaced in the tool schema. |
| `host`      | `host[:port]` of the box (or the proxy in front of it). |
| `auth`      | HTTP Basic credentials as `user:pass`. Write-only — never returned by `instances get`. |
| `scheme`    | `http` or `https` (default `https`). |
| `insecure`  | `true` accepts a self-signed cert — the JSON equivalent of `curl -k`. |
| `allow_rpc` | `true` opts this box into the `rpc` escape hatch. Hand-edit only. |

The file is created `0600` in a `0700` directory (it holds passwords) and is
written back atomically when the `instances` tool changes it.

**No config file?** A single box can be defined entirely from the environment,
applied to an implicit `default` instance: `KODI_HOST`, `KODI_AUTH`,
`KODI_SCHEME`, and `-k` in `KODI_CURL_OPTS` (→ `insecure`). With neither a file
nor env, the server exits with a clear error telling you to configure.

A legacy `version: 1` flat file (single top-level `host`/`auth`/…) is read as one
instance named `default` and rewritten in the `version: 2` shape on the next save.

> **Note:** config is read **once at startup**. After hand-editing `config.json`
> (e.g. to set `allow_rpc`), restart the server so it re-reads the file.

---

## Kodi server setup

Kodi must have its JSON-RPC web server turned on, and — for normal remote use —
an HTTPS reverse proxy in front of it (mcp-kodi speaks HTTPS). A step-by-step
guide, written for non-server-admins, is in
[docs/kodi-server-setup.md](docs/kodi-server-setup.md).

The short version: enable **Settings → Services → Control → Allow remote
control via HTTP**, then put [Caddy](https://caddyserver.com) (with `tls
internal`) in front of Kodi's port 8080 and point `host` at the proxy with
`insecure: true`.

---

## Registering with an MCP client

### Claude Code

```sh
# user scope makes the tools available in every project
claude mcp add --scope user kodi -- /path/to/mcp-kodi/src/mcp-kodi
```

### Claude Desktop / other MCP clients

Add a stdio server entry pointing at the binary:

```json
{
  "mcpServers": {
    "kodi": {
      "command": "/path/to/mcp-kodi/src/mcp-kodi"
    }
  }
}
```

Once registered, ask your assistant things like *"what's the player doing in the
living room?"* (`noop`), *"find Pink Floyd's Animals and play it"* (`search` →
`playfile`), or *"mute the bedroom"* (`mute`).

---

## Security notes

- `config.json` stores Kodi passwords in plaintext, mode `0600`. Protect it like
  any other credential file.
- `insecure: true` disables TLS certificate verification for that box. It is the
  right choice behind Caddy's `tls internal` self-signed cert, but means the
  connection is encrypted, not authenticated against a CA — keep the proxy on a
  trusted network.
- The `rpc` escape hatch can invoke **any** JSON-RPC method (including
  destructive ones like `System.Shutdown`). It stays off until you deliberately
  set `allow_rpc: true` for a specific box. Enable it only where you trust the
  assistant to operate unconstrained.

---

## Design & internals

The full design spec — protocol handling, tool semantics, config format, and the
live-Kodi captures behind each tool — lives in [TODO.md](TODO.md). Tool surface
and Kodi method references: [docs/](docs/).

---

## License

Copyright © 2026 Laszlo Pere &lt;laszlopere@gmail.com&gt;. An independent,
clean-room implementation.
