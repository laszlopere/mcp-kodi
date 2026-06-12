# Container image for the Glama directory health-check.
#
# Glama only needs the server to start and answer MCP introspection
# (tools/list) inside a container — it never talks to a real Kodi. The server
# boots with no config.json and no env (tool *calls* then fail gracefully with
# a no_instance error), so no dummy configuration is needed. See TODO.txt
# §11.26.
#
# Debian trixie ships glib 2.80+, json-glib 1.6+ and libsoup-3.0, matching the
# Build-Depends in debian/control.

# --- build stage -----------------------------------------------------------
FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        autoconf \
        automake \
        libtool \
        pkgconf \
        libglib2.0-dev \
        libjson-glib-dev \
        libsoup-3.0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN ./autogen.sh && ./configure && make

# --- runtime stage ---------------------------------------------------------
FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libglib2.0-0 \
        libjson-glib-1.0-0 \
        libsoup-3.0-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /app/src/mcp-kodi /app/src/mcp-kodi

CMD ["/app/src/mcp-kodi"]
