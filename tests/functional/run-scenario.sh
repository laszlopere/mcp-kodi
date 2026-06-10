#!/usr/bin/env bash
#
# mcp-kodi — interactive functional test runner (talks to Kodi directly).
#
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
#
# This is NOT an MCP-server test. It POSTs raw JSON-RPC requests straight to a
# Kodi instance so we can observe Kodi's real behaviour before changing the MCP
# code. It reads a line-oriented scenario file, sends each request UNMODIFIED,
# waits a few seconds, probes the player state, reports it against the step's
# expected state, and either prompts the operator (y/n) or — in single-step
# mode — reports one step and exits so Claude Code can drive the loop.
#
# Scenario file format (line-oriented; see scenarios/*.txt):
#   # title             a comment / step title (optional, attaches to next req)
#   {"jsonrpc":...}      a line beginning with '{' is a JSON-RPC request, sent verbatim
#   [state] one-liner    the line AFTER a request: what to observe on screen.
#                        An optional leading [state] (playing|paused|stopped)
#                        drives the automatic PASS/FAIL check; the rest is the
#                        human-readable observation. Blank lines separate steps.
#
# Usage:
#   run-scenario.sh [-i INSTANCE] [-w SECONDS] [-s INDEX] [-n] SCENARIO
#
#   -i INSTANCE   config instance name to target (default: local)
#   -w SECONDS    settle delay between sending a request and probing (default: 3)
#   -s INDEX      run only step INDEX (1-based) and exit, WITHOUT the y/n prompt
#                 (used when Claude Code drives the scenario one step at a time)
#   -n            print how many steps the scenario has, then exit
#   SCENARIO      path to the scenario file
#
# Env:
#   MCP_KODI_CONFIG  path to config.json (default: ~/.config/mcp-kodi/config.json)
#   AUTO=1           don't prompt; auto-answer 'y' (for unattended smoke runs)
#
# Connection details (scheme/host/auth/insecure) are read from the config file
# for the chosen instance — never hardcoded.

set -u

instance="local"
wait_s="3"
only_step=""
count_only="0"
config="${MCP_KODI_CONFIG:-$HOME/.config/mcp-kodi/config.json}"

die () { printf 'error: %s\n' "$*" >&2; exit 1; }

while getopts ":i:w:s:nh" opt; do
  case "$opt" in
    i) instance="$OPTARG" ;;
    w) wait_s="$OPTARG" ;;
    s) only_step="$OPTARG" ;;
    n) count_only="1" ;;
    h) sed -n '2,40p' "$0"; exit 0 ;;
    *) die "bad option; run with -h" ;;
  esac
done
shift $((OPTIND - 1))

scenario="${1:-}"
[ -n "$scenario" ] || die "no scenario file given (try -h)"
[ -r "$scenario" ] || die "cannot read scenario: $scenario"
[ -r "$config" ]   || die "cannot read config: $config"
command -v jq   >/dev/null || die "jq not found"
command -v curl >/dev/null || die "curl not found"

# --- resolve the instance's connection from config -------------------------
scheme=$(jq -r --arg i "$instance" '.instances[$i].scheme // "http"'  "$config")
host=$(  jq -r --arg i "$instance" '.instances[$i].host   // empty'   "$config")
auth=$(  jq -r --arg i "$instance" '.instances[$i].auth   // empty'   "$config")
insecure=$(jq -r --arg i "$instance" '.instances[$i].insecure // false' "$config")
[ -n "$host" ] || die "instance '$instance' not found (or has no host) in $config"

url="$scheme://$host/jsonrpc"
curl_opts=(-s --max-time 10 -H 'Content-Type: application/json')
[ -n "$auth" ]            && curl_opts+=(-u "$auth")
[ "$insecure" = "true" ] && curl_opts+=(-k)

# --- parse the line-oriented scenario into parallel arrays -----------------
declare -a S_TITLE S_REQ S_STATE S_OBS
title=""; req=""
add_step () { S_TITLE+=("$1"); S_REQ+=("$2"); S_STATE+=("$3"); S_OBS+=("$4"); }
while IFS= read -r line || [ -n "$line" ]; do
  trimmed="${line#"${line%%[![:space:]]*}"}"          # strip leading whitespace
  [ -z "$trimmed" ] && continue                       # blank line
  case "$trimmed" in
    \#*) title="${trimmed#\#}"; title="${title# }" ;; # comment -> next step's title
    \{*) req="$trimmed" ;;                             # JSON-RPC request (verbatim)
    *)                                                 # observation line for $req
       [ -n "$req" ] || die "expectation line with no preceding request: $trimmed"
       state=""; obs="$trimmed"
       case "$trimmed" in
         \[*\]*) state="${trimmed#\[}"; state="${state%%\]*}"
                 obs="${trimmed#*\]}"; obs="${obs# }" ;;
       esac
       add_step "$title" "$req" "$state" "$obs"
       title=""; req="" ;;
  esac
done < "$scenario"
nsteps=${#S_REQ[@]}
[ "$nsteps" -gt 0 ] || die "scenario has no steps"

# kodi_call <json-request> -> raw JSON response on stdout
kodi_call () { curl "${curl_opts[@]}" -d "$1" "$url"; }

# probe_state -> prints a one-line summary; sets global STATE word
#   STATE is one of: stopped | playing | paused
probe_state () {
  local active players pid speed item label
  active=$(kodi_call '{"jsonrpc":"2.0","id":1,"method":"Player.GetActivePlayers"}')
  players=$(printf '%s' "$active" | jq -r '(.result // []) | length' 2>/dev/null)
  if [ "${players:-0}" = "0" ]; then
    STATE="stopped"; printf 'state=stopped (no active player)\n'; return
  fi
  pid=$(printf '%s' "$active" | jq -r '.result[0].playerid')
  speed=$(kodi_call "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Player.GetProperties\",\"params\":{\"playerid\":$pid,\"properties\":[\"speed\"]}}" \
            | jq -r '.result.speed // 0')
  item=$(kodi_call "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Player.GetItem\",\"params\":{\"playerid\":$pid,\"properties\":[\"title\"]}}")
  label=$(printf '%s' "$item" | jq -r '.result.item.label // .result.item.title // "?"')
  if [ "$speed" != "0" ]; then STATE="playing"; else STATE="paused"; fi
  printf 'state=%s  playerid=%s  speed=%s  item="%s"\n' "$STATE" "$pid" "$speed" "$label"
}

# run_step <0-based index> <total> -> prints the step report; returns 0 if the
# state check passed (or there was none), 1 if it FAILed. Sends no y/n prompt.
run_step () {
  local i="$1" total="$2" name request want expect resp
  name="${S_TITLE[$i]:-}"; [ -n "$name" ] || name="step $((i+1))"
  request="${S_REQ[$i]}"            # sent verbatim
  want="${S_STATE[$i]}"
  expect="${S_OBS[$i]}"

  printf -- '--- step %s/%s: %s ---\n' "$((i+1))" "$total" "$name"
  printf 'REQUEST  %s\n' "$request"
  resp=$(kodi_call "$request")
  printf 'RESPONSE %s\n' "$(printf '%s' "$resp" | jq -c '.result // .error // .' 2>/dev/null || printf '%s' "$resp")"

  printf 'settling %ss...\n' "$wait_s"
  sleep "$wait_s"
  probe_state

  printf 'EXPECT ON SCREEN: %s\n' "$expect"
  if [ -n "$want" ]; then
    if [ "$STATE" = "$want" ]; then
      printf 'CHECK    PASS (expected %s)\n' "$want"; return 0
    fi
    printf 'CHECK    FAIL (expected %s, got %s)\n' "$want" "$STATE"; return 1
  fi
  return 0
}

# --- entry points ----------------------------------------------------------
# -n: just report the step count (lets a driver know how many steps to run).
if [ "$count_only" = "1" ]; then
  printf '%s\n' "$nsteps"; exit 0
fi

# -s INDEX: single-step mode (Claude Code drives the loop; no y/n prompt).
if [ -n "$only_step" ]; then
  [ "$only_step" -ge 1 ] 2>/dev/null && [ "$only_step" -le "$nsteps" ] \
    || die "step out of range: $only_step (scenario has $nsteps)"
  printf 'instance=%s  url=%s  settle=%ss\n' "$instance" "$url" "$wait_s"
  run_step $((only_step - 1)) "$nsteps"
  exit $?
fi

# Default: full interactive run, prompting the operator at the terminal.
printf '=== scenario: %s ===\n' "$scenario"
printf 'instance=%s  url=%s  settle=%ss  steps=%s\n\n' "$instance" "$url" "$wait_s" "$nsteps"

pass=0; fail=0
for i in $(seq 0 $((nsteps - 1))); do
  if run_step "$i" "$nsteps"; then pass=$((pass+1)); else fail=$((fail+1)); fi

  if [ "${AUTO:-0}" = "1" ]; then
    printf 'AUTO: continuing\n\n'; continue
  fi
  printf 'Looks right on screen? [y/N] '
  read -r ans < /dev/tty
  case "$ans" in
    y|Y) printf '\n' ;;
    *)   printf 'aborted by operator at step %s\n' "$((i+1))"; exit 2 ;;
  esac
done

printf '=== done: %s passed, %s failed (of %s steps) ===\n' \
  "$pass" "$fail" "$nsteps"
[ "$fail" -eq 0 ]
