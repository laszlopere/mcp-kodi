# mcp-kodi

An [MCP](https://modelcontextprotocol.io) server that lets an AI assistant
control a [Kodi](https://kodi.tv) media player over Kodi's JSON-RPC API. Written
in C on the GLib stack.

There is **no user-facing CLI**: you talk to the AI, the AI calls this server's
tools, and the server speaks JSON-RPC to Kodi. The only interface is MCP over
stdio.

**Where this is going.** The plan is a complete, full-featured package that lets
any MCP-compatible AI transform the entire media-player experience — not just
press buttons, but reinvent how you discover, queue, and enjoy your media. Tell
the assistant what you're in the mood for and let it run the room: build the
evening's lineup, pick up where you left off last night, adapt on the fly, and
surface things you'd never have found yourself. We're aiming for something
genuinely new — a mind-blowingly different way to live with your media player.

> **Status — early, but already remarkable (v0.2.0dev).** A growing set of
> dedicated, purpose-built tools covers transport, volume, search, queue
> management, and playback history. What makes the build capable *beyond* that
> list is the **`rpc` escape hatch** (see below): an opt-in passthrough to *any*
> Kodi JSON-RPC method. Between the first-class tools and that escape hatch, an
> assistant can already drive nearly everything Kodi exposes — remarkable enough
> that we decided to publish early rather than wait for the full vision to land.
> Expect frequent releases.

---

## What it can do

Each tool targets a configured Kodi box by key (`instance`); omit it and the
configured `default` box is used (with one exception: `history` spans **all**
boxes when `instance` is omitted).

**Transport & audio**

| Tool       | What it does |
|------------|--------------|
| `play`     | Press **Play** on the remote — resume or begin playback on the target box. |
| `pause`    | Press **Pause** — pause the active player. |
| `stop`     | Press **Stop** — stop the active player **and clear the playlist it was consuming**, so no queued items linger. |
| `volume`   | Adjust the volume by a **relative** signed step (percentage points); step `0`/omitted just reads. Never an absolute level, so the assistant can't blast or silence the box by guessing. Returns volume, mute state, and the `0`–`100` bounds. |
| `mute`     | Mute the box's audio output. |
| `unmute`   | Unmute the box's audio output. |
| `nowplaying` | Report what is playing without changing anything — also a reachability + state probe. |

**Search & discovery**

| Tool           | What it does |
|----------------|--------------|
| `searchmedia`  | Find playable **leaf files** by name across `music` / `tv-show` / `movie`, with paging (`limit`/`offset`) and a total count. Music `title` matches the **album** — songs cannot be searched by their own name. Movie/tv-show queries can also filter by `actor`/`director`. Resolves the library by name, so the assistant acts by title, not numeric id. Media items only — people lookups live in `contributors`. |
| `contributors` | Find or list **people** — bands, solo artists, composers, actors, directors — by optional name substring and/or `type`. Each row says where the name yields hits (`albums`/`songs`/`movies`/`tvshows`); feed the exact name back into `searchmedia` to drill. |

**Playback & queue**

| Tool            | What it does |
|-----------------|--------------|
| `playfile`      | Play one file by path (typically a `searchmedia` result's `file`). Kodi auto-selects the audio/video player. Works for any reachable path, in-library or not. A file missing from disk (stale library entry) is reported as an error. |
| `queue`         | Append an item behind the one now playing (a `searchmedia` library id `type`+`id`, or a `file` path); `next: true` inserts it right after the current item. Something must already be playing. A file missing from disk is refused. |
| `getplaylist`   | Read a queue without changing it — the active player's playlist, or a named one (`audio`/`video`/`picture`), plus the now-playing position. Read-only; an empty queue is an empty list. |
| `dropplaylists` | Empty **all** queues (audio, video, picture) in one call. The current item keeps playing — only the items queued behind it are removed. Not undoable; inspect with `getplaylist` first if the content matters. |

**Bookkeeping & escape hatch**

| Tool        | What it does |
|-------------|--------------|
| `history`   | List recently played items from the **local** playback log, written as a side effect of every playback-affecting call. Optional ISO-8601 `since`/`until` window plus app-side filters — `media`, `kind`, `artist`, free-text `match`, exact `id` — all AND-combined, with `limit`/`offset`/`order` paging and a `count`-only mode. Reads only the local log — no Kodi call, so it works even when no box is reachable. Omitted `instance` returns **all** boxes. |
| `instances` | Read or modify the server's own instance config (`get`/`set`/`remove`). Makes no Kodi call; never returns stored passwords. |
| `rpc`       | **Escape hatch** — send a raw JSON-RPC method to Kodi and return its reply unchanged. Off by default; opt-in per instance (see below). |

Most action tools return a small player-state snapshot — `{ "state":
"playing"|"paused"|"stopped", "type", "media", "id", "file", "label", "title",
"time", "totaltime" }`, plus per-media fields where they apply (`artist`,
`album`, `track`, `showtitle`, `season`, `episode`) —
so the assistant always sees the effect of its action: this covers
`play`/`pause`/`stop`, `playfile`, `queue`, `dropplaylists`, and `nowplaying`. The
audio tools `volume`/`mute`/`unmute` return `{ "muted", "volume" }` (`volume`
also adds the `min`/`max` bounds). The read tools (`searchmedia`,
`contributors`, `getplaylist`, `history`) return their own paged result
envelopes.

Every tool except `rpc` declares these shapes as an MCP `outputSchema`
(spec revision 2025-06-18) and mirrors each successful result as
`structuredContent` alongside the JSON text block, so schema-aware clients
can validate and consume results without parsing. Each description also
states the result shape inline — the channel every client shows the model.
`rpc` declares none: it returns Kodi's raw reply verbatim, which has no
fixed shape.

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

## Configuration

### Set up Kodi for remote control

Before mcp-kodi can reach a box, that Kodi has to allow remote control. In Kodi,
open **Settings → Services → Control** and turn on **Allow remote control via
HTTP**, then set a **username and password** right there. Those same credentials
go into this server's config as `auth`, in `user:pass` form.

mcp-kodi speaks **HTTPS**, so for normal use you put a small reverse proxy in
front of Kodi's plain-HTTP control port. [Caddy](https://caddyserver.com) with
`tls internal` is the easy choice: it terminates HTTPS and forwards to Kodi on
`localhost`. Point the instance's `host` at the proxy and set `insecure: true`
to accept its self-signed certificate. The full step-by-step walkthrough — the
exact Kodi menus, installing Caddy, and the Caddyfile — is in
[docs/kodi-server-setup.md](docs/kodi-server-setup.md).

Most Kodi players already sit on a home LAN behind a router/firewall and are not
exposed to the internet, so turning on the HTTP control interface there is
generally safe. The login/password and the HTTPS proxy add defence in depth —
just keep the proxy on a trusted network and don't forward its port to the open
internet.

### Registering a Kodi instance

There are two ways to add a box. The simplest is to **let the AI do it**: once
the MCP server is loaded, just tell the assistant about your Kodi — its address
and login — and it registers the instance for you through the `instances` tool
(`set`), writing the entry into the config file.

You can also **do it by hand**, by editing `config.json` directly. Two things
are deliberately *off-limits* to the AI, so hand-editing is the only way to set
them:

- **The password.** `auth` is write-only — the `instances` tool never returns a
  stored password, so the assistant can register a box but can never read back
  the credentials. If you'd rather the AI never see the password at all, type it
  into the file yourself.
- **The escape hatch.** `allow_rpc` is **not** a field the `instances` tool can
  write, so the assistant can never grant itself the unrestricted `rpc`
  passthrough. Enabling it is always an explicit, out-of-band human edit.

### The config file

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
living room?"* (`nowplaying`), *"find Pink Floyd's Animals and play it"* (`searchmedia` →
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

## License

Copyright © 2026 Laszlo Pere &lt;laszlopere@gmail.com&gt;. An independent,
clean-room implementation.

Free software, licensed under the **GNU General Public License v3.0**. You may
redistribute and/or modify it under those terms; it comes with no warranty. See
[LICENSE](LICENSE) for the full text.

---

## Support this project

If this project is useful to you, please consider supporting it through
[GitHub Sponsors](https://github.com/sponsors/laszlopere). Every bit of support
keeps it alive — funding the time to ship the frequent releases this is built
on, and to chase the full vision.

And if you're still on the fence: ask your Claude whether this project is good
for you — and if it recommends it (as most Claude do), support it by becoming a
sponsor.
