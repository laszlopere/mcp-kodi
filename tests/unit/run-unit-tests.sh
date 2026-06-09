#!/usr/bin/env bash
#
# mcp-kodi — build and run the C unit tests.
#
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laszlo Pere <laszlopere@gmail.com>
#
# Compiles each unit-test binary from its own sources (the module under test
# plus stubs for anything that would otherwise do file or network I/O), runs it,
# and reports how many tests failed and which ones. Each test binary exits 0 when
# every check passed and non-zero otherwise, which is what we key off here.
#
# Usage:
#   ./run-unit-tests.sh            # tabular summary per test
#   ./run-unit-tests.sh --verbose  # also print every individual check
#   ./run-unit-tests.sh -k         # keep the build dir (default: removed)

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
top="$(cd "$here/../.." && pwd)"
src="$top/src"
build="$here/build"

verbose=""
keep=0
for arg in "$@"; do
  case "$arg" in
    --verbose|-v) verbose="--verbose" ;;
    -k|--keep)    keep=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

CC="${CC:-cc}"
PKGS="glib-2.0 gobject-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0"

if ! pkg-config --exists $PKGS; then
  echo "error: required pkg-config modules missing: $PKGS" >&2
  exit 2
fi

PKG_CFLAGS="$(pkg-config --cflags $PKGS)"
PKG_LIBS="$(pkg-config --libs $PKGS)"
# -I$top for config.h, -I$src for the module headers.
CFLAGS="-I$here -I$top -I$src -Wall -Wextra -g -O0 $PKG_CFLAGS"

mkdir -p "$build"

# test name  ->  space-separated source list (relative to $here unless absolute)
declare -A SOURCES=(
  [test-stdio]="test-stdio.c $src/mk-stdio.c"
  [test-config]="test-config.c $src/mk-config.c"
  [test-config-io]="test-config-io.c $src/mk-config.c"
  [test-history]="test-history.c $src/mk-history.c"
  [test-tools]="test-tools.c $src/mk-tools.c $src/mk-config.c stub-kodi.c stub-history.c"
  [test-tools-handlers]="test-tools-handlers.c $src/mk-tools.c $src/mk-config.c stub-kodi-prog.c stub-history.c"
)

# Deterministic order.
TESTS="test-stdio test-config test-config-io test-history test-tools test-tools-handlers"

failed=0
failed_list=()

for t in $TESTS; do
  srcs="${SOURCES[$t]}"
  # Resolve bare source names against $here.
  objs=()
  for s in $srcs; do
    case "$s" in
      /*) objs+=("$s") ;;
      *)  objs+=("$here/$s") ;;
    esac
  done

  bin="$build/$t"
  echo "=== building $t ==="
  if ! $CC $CFLAGS "${objs[@]}" -o "$bin" $PKG_LIBS; then
    echo "  BUILD FAILED: $t" >&2
    failed=$((failed + 1))
    failed_list+=("$t (build)")
    continue
  fi

  echo "=== running $t ==="
  if "$bin" $verbose; then
    :
  else
    failed=$((failed + 1))
    failed_list+=("$t")
  fi
  echo
done

echo "================================================================"
if [ "$failed" -eq 0 ]; then
  echo "All tests passed."
else
  echo "$failed test(s) failed:"
  for f in "${failed_list[@]}"; do
    echo "  - $f"
  done
fi
echo "================================================================"

if [ "$keep" -eq 0 ]; then
  rm -rf "$build"
fi

exit "$failed"
