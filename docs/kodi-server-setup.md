# Setting up Kodi for mcp-kodi

This guide walks you through preparing your Kodi box so this project can control
it. No prior server experience is assumed — if you can edit a setting in Kodi's
menus and paste a few commands into a terminal, you can do this.

You will do two things:

1. **Turn on Kodi's remote-control interface** (the JSON-RPC web server). This is
   a couple of clicks in Kodi's settings.
2. **Put a small HTTPS reverse proxy (Caddy) in front of it**, so the connection
   is encrypted instead of plain text.

Step 2 is optional but strongly recommended. If you only ever talk to Kodi from
the same machine, you could skip it — but mcp-kodi is built to speak **HTTPS**,
so for normal use you want the proxy.

---

## How it fits together

```
  mcp-kodi  ──HTTPS──►  Caddy reverse proxy  ──plain HTTP──►  Kodi web server
 (this app)            (e.g. host:8443, TLS)                 (127.0.0.1:8080)
                              │
                              └──WebSocket──►  Kodi JSON-RPC socket (127.0.0.1:9090)
```

- Kodi itself only speaks **unencrypted HTTP** on port **8080**, plus a separate
  WebSocket channel on **9090**. Those are kept on the local machine.
- **Caddy** listens on **8443** with a TLS certificate and forwards everything to
  Kodi. The outside world only ever talks to Caddy, over HTTPS.
- mcp-kodi connects to Caddy using the host, port, and login you configure.

You don't have to understand WebSockets or TLS to follow the steps — just copy
the configuration as shown.

---

## Part 1 — Turn on Kodi's web server

This is done entirely inside Kodi, with the remote or a keyboard.

1. Open **Settings** (the gear icon on the home screen).
2. Go to **Services → Control**.
3. Set the following:

   | Setting                              | Value             |
   | ------------------------------------ | ----------------- |
   | **Allow remote control via HTTP**    | **ON**            |
   | **Port**                             | **8080**          |
   | **Require authentication**           | **ON**            |
   | **Username**                         | `kodi`            |
   | **Password**                         | *(choose one)*    |

   > **Pick your own password.** You'll enter the same username and password into
   > mcp-kodi's config later. Avoid characters like `:` `@` `/` in the password —
   > they can confuse URLs. A simple word or phrase is fine for a home network.

4. While you're on the same screen, also turn **ON**:

   | Setting                                          | Value  |
   | ------------------------------------------------ | ------ |
   | **Allow remote control from applications on this system**  | **ON** |
   | **Allow remote control from applications on other systems** | **ON** |

   These enable the JSON-RPC **WebSocket** channel (port **9090**) that the proxy
   uses. Leaving "other systems" on is harmless here because, as set up below,
   only the local Caddy proxy actually connects to it.

That's it for Kodi. The web server starts immediately — no reboot needed.

### Quick local test

From a terminal **on the Kodi machine**, check Kodi answers (replace
`YOURPASSWORD`):

```bash
curl -s -u kodi:YOURPASSWORD \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"JSONRPC.Ping"}' \
  http://127.0.0.1:8080/jsonrpc
```

You should see:

```json
{"id":1,"jsonrpc":"2.0","result":"pong"}
```

If you get `pong`, Kodi is ready. If you get a 401, the username/password don't
match what you set. If nothing connects, double-check "Allow remote control via
HTTP" is on and the port is 8080.

---

## Part 2 — Put Caddy (HTTPS) in front of Kodi

[Caddy](https://caddyserver.com) is a tiny, friendly web server. We use it for
one job: accept HTTPS on port 8443 and forward to Kodi. It can generate its own
certificate automatically, so you don't have to buy or configure one.

### 2.1 Install Caddy

On Debian/Ubuntu:

```bash
sudo apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
  | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
  | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update && sudo apt install -y caddy
```

(For other systems, see <https://caddyserver.com/docs/install>.)

Installing Caddy this way also sets it up as a background service that starts on
boot.

### 2.2 Write the configuration

Caddy reads a single file called the **Caddyfile**, at `/etc/caddy/Caddyfile`.
Replace its contents with the following. **Change `kodi.example.local` to your
machine's hostname or IP address** (you can list several, comma-separated):

```caddyfile
kodi.example.local:8443, 192.168.1.50:8443 {
    # Kodi serves JSON-RPC over WebSocket on a separate port (9090).
    # Match WebSocket upgrade requests and send them there...
    @websockets {
        header Connection *Upgrade*
        header Upgrade    websocket
    }
    reverse_proxy @websockets 127.0.0.1:9090

    # ...everything else (normal JSON-RPC over HTTP) goes to Kodi's web server.
    reverse_proxy           127.0.0.1:8080

    # Generate and manage a self-signed certificate automatically.
    tls internal
}
```

To edit the file:

```bash
sudo nano /etc/caddy/Caddyfile     # paste, then Ctrl-O Enter, Ctrl-X to save
```

> **What is `tls internal`?** It tells Caddy to make its own certificate from a
> private certificate authority, instead of getting one from a public authority
> like Let's Encrypt. That's exactly what you want for a home device that isn't
> on the public internet. The trade-off: programs that connect to it must be told
> to **skip certificate verification** (mcp-kodi does this for you), because the
> certificate isn't signed by an authority your system already trusts.

### 2.3 Start Caddy

```bash
sudo systemctl reload caddy        # apply the new Caddyfile
sudo systemctl enable --now caddy  # start now and on every boot
```

Check it's running:

```bash
systemctl status caddy
```

### 2.4 Test through the proxy

From **any machine on your network** (replace host and password):

```bash
curl -sk -u kodi:YOURPASSWORD \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"JSONRPC.Ping"}' \
  https://kodi.example.local:8443/jsonrpc
```

The `-k` flag tells `curl` to accept the self-signed certificate. You should get
`{"id":1,"jsonrpc":"2.0","result":"pong"}` again — but this time over HTTPS,
through Caddy.

---

## Part 3 — Point mcp-kodi at your server

mcp-kodi reads its connection details from `~/.config/kodi/cli.conf`. Create that
file with your values:

```bash
# kodi connection — used by mcp-kodi
KODI_HOST="kodi.example.local:8443"   # your hostname/IP : Caddy port
KODI_AUTH="kodi:YOURPASSWORD"         # username:password from Part 1
KODI_SCHEME="https"
KODI_CURL_OPTS="-sk"                  # -k accepts the self-signed cert
```

That's the whole setup. mcp-kodi will now reach Kodi over encrypted HTTPS.

---

## Troubleshooting

| Symptom                                   | Likely cause / fix                                                                 |
| ----------------------------------------- | ---------------------------------------------------------------------------------- |
| `pong` on 8080 but proxy times out        | Caddy isn't running, or the Caddyfile hostname/port don't match. `systemctl status caddy`, check `/etc/caddy/Caddyfile`. |
| `401 Unauthorized`                        | Username/password mismatch between Kodi's settings and your `KODI_AUTH`.            |
| `certificate ... not trusted` errors      | Expected with `tls internal`. Make sure your client uses `-k` / skips verification (mcp-kodi's `KODI_CURL_OPTS="-sk"`). |
| WebSocket calls fail but HTTP works        | The two "Allow remote control from applications…" toggles (Part 1, step 4) are off, so port 9090 isn't accepting connections. |
| Works locally, not from other machines     | A firewall is blocking port 8443. Allow it, e.g. `sudo ufw allow 8443/tcp`.        |
| Caddy won't start after editing            | Syntax error in the Caddyfile. Run `caddy validate --config /etc/caddy/Caddyfile` to see the line. |

---

## Appendix — Reference configuration (the author's working setup)

A complete, working example for comparison. Substitute your own hostname/IP and
password.

**Versions:** Kodi 19.4 (Matrix) on Ubuntu 22.04 · Caddy v2.11.3

**Kodi** — relevant entries in `~/.kodi/userdata/guisettings.xml`:

```xml
<setting id="services.webserver">true</setting>
<setting id="services.webserverport">8080</setting>
<setting id="services.webserverauthentication">true</setting>
<setting id="services.webserverusername">kodi</setting>
<setting id="services.webserverpassword">…</setting>
```

Kodi listens on `0.0.0.0:8080` (HTTP web server) and `127.0.0.1:9090`
(JSON-RPC WebSocket / TCP announce port).

**Caddy** — `/etc/caddy/Caddyfile`:

```caddyfile
kodi.example.local:8443, 192.168.1.50:8443 {
    @websockets {
        header Connection *Upgrade*
        header Upgrade    websocket
    }
    reverse_proxy @websockets 127.0.0.1:9090
    reverse_proxy           127.0.0.1:8080
    tls internal
}
```

Caddy listens on `*:8443`. Restrict access to your trusted LAN (host firewall
and/or router rules) so only the proxy port is reachable.
