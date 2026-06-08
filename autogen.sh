#!/bin/sh
# Bootstrap the autotools build system.
set -e

srcdir=$(dirname "$0")
cd "$srcdir"

mkdir -p m4 build-aux

autoreconf --install --verbose --force

echo "Now run ./configure && make"
