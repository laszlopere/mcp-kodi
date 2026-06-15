# shellcheck shell=sh
# tests/lib.sh — shared harness for the mcp-kodi functional tests.
#
# POSIX /bin/sh; *sourced* (not executed) by each *.test right after `set -u`:
#
#     . "$(dirname "$0")/lib.sh"
#
# It centralises the boilerplate every functional test would otherwise repeat:
# the target-Kodi defaults, the dependency/binary/liveness skip gates, the
# direct-to-Kodi JSON-RPC wrapper, the MCP-over-stdio tool caller, the jq
# payload helpers, and the ok/no/eq assertion mini-framework.
#
# Contract for callers:
#   . lib.sh                 # sets KODI_HOST/AUTH/SCHEME, SKIP, PASS=FAIL=0
#   mk_preflight             # jq+curl, locate binary into $MCP, require ping
#   ... discovery + assertions ...
#   mk_tally "<test name>"   # prints the pass/fail line and exits 0/1
#
# Tests that do not want the live-Kodi gate (e.g. config-only tests) call the
# finer-grained mk_require_tools / find_binary / mk_require_ping themselves
# instead of the mk_preflight bundle.

# ---- target Kodi (override via environment) -------------------------------
KODI_HOST=${KODI_HOST:-127.0.0.1:8080}
KODI_AUTH=${KODI_AUTH:-kodi:kodi}
KODI_SCHEME=${KODI_SCHEME:-http}
export KODI_HOST KODI_AUTH KODI_SCHEME

SKIP=77
skip () { echo "SKIP: $*"; exit $SKIP; }

# ---- dependencies + binary ------------------------------------------------
mk_require_tools () {
  command -v jq   >/dev/null 2>&1 || skip "jq not found"
  command -v curl >/dev/null 2>&1 || skip "curl not found"
}

# Locate the mcp-kodi binary: the automake build tree first ($top_builddir is
# exported by `make check`), then fallbacks for running the script by hand.
find_binary () {
  for c in \
    "${top_builddir:-}/src/mcp-kodi" \
    "$(dirname "$0")/../src/mcp-kodi" \
    "./src/mcp-kodi" \
    "src/mcp-kodi"
  do
    if [ -n "$c" ] && [ -x "$c" ]; then printf '%s\n' "$c"; return 0; fi
  done
  return 1
}

# ---- talking to Kodi directly (for discovery + the liveness gate) ---------
kodi_rpc () { # method params-json  ->  raw JSON-RPC response
  curl -s -m 20 -u "$KODI_AUTH" -H 'Content-Type: application/json' \
    --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" \
    "$KODI_SCHEME://$KODI_HOST/jsonrpc"
}

mk_require_ping () {
  pong=$(kodi_rpc JSONRPC.Ping '{}' | jq -r '.result // empty' 2>/dev/null)
  [ "$pong" = "pong" ] || skip "Kodi not reachable at $KODI_SCHEME://$KODI_HOST"
}

# Dependency + binary + liveness gate, in the order every playback test wants.
# Sets the global $MCP to the located binary.
mk_preflight () {
  mk_require_tools
  MCP=$(find_binary) || skip "mcp-kodi binary not built"
  mk_require_ping
}

# ---- talking to the server under test (MCP over stdio) --------------------
# Feed `initialize` + one `tools/call`, echo back the tool result object for
# request id 2 (`{ content:[{type,text}], isError }`). The server reads
# newline-delimited JSON and exits on EOF, so one invocation per call keeps
# every assertion isolated. Requires $MCP to be set (see find_binary).
mcp_tool () { # name arguments-json  ->  tool result object
  printf '%s\n%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{}}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"$1\",\"arguments\":$2}}" \
    | "$MCP" 2>/dev/null \
    | jq -c 'select(.id==2) | .result'
}

# ---- JSON payload helpers -------------------------------------------------
res_err  () { printf '%s' "$1" | jq -r '.isError'; }                       # "true"|"false"
res_body () { printf '%s' "$1" | jq -c '.content[0].text | fromjson'; }    # tool payload (JSON)
res_text () { printf '%s' "$1" | jq -r '.content[0].text'; }              # raw text payload
jargs    () { jq -cn "$@"; }                  # build an arguments object safely
g        () { printf '%s' "$1" | jq -r "$2"; } # body, filter -> scalar
j        () { printf '%s' "$1" | jq    "$2"; } # body, filter -> json/bool

# ---- throwaway state dir --------------------------------------------------
# Redirect history.json (under XDG_STATE_HOME) to a throwaway dir so a run never
# touches the user's real ~/.local/state, and arm an EXIT trap to clean it up.
# Tests that need extra teardown (e.g. a volume guard) should set their own
# combined trap *after* this and reuse $STATE_DIR.
mk_state_dir () {
  STATE_DIR=$(mktemp -d) || skip "cannot mktemp"
  export XDG_STATE_HOME="$STATE_DIR"
  trap 'rm -rf "$STATE_DIR"' EXIT
}

# ---- assertions -----------------------------------------------------------
PASS=0
FAIL=0
ok () { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
no () { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; shift; [ $# -gt 0 ] && printf '       %s\n' "$*"; return 0; }

eq () { # desc expected actual
  if [ "x$2" = "x$3" ]; then ok "$1"; else no "$1" "expected [$2] got [$3]"; fi
}
is_true () { # desc jq-bool-string
  if [ "x$2" = "xtrue" ]; then ok "$1"; else no "$1" "condition was [$2]"; fi
}
contains () { # desc needle haystack
  case "$3" in *"$2"*) ok "$1";; *) no "$1" "[$3] has no [$2]";; esac
}

# ---- tally ----------------------------------------------------------------
mk_tally () { # test-name
  echo "------------------------------------------------------------"
  printf '%s: %d passed, %d failed\n' "${1:-functional test}" "$PASS" "$FAIL"
  [ "$FAIL" -eq 0 ] || exit 1
  exit 0
}
