#!/usr/bin/env bash
set -euo pipefail

# Dynamically locate the script root context
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SRC="$SCRIPT_DIR/sprx-plugin"
DST="$SCRIPT_DIR/sprx-plugin/build"

TMPDIR=/tmp/ldtoypad-build
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"

mkdir -p "$DST"

cp "$SRC"/*.c "$TMPDIR/"
cp "$SRC"/*.h "$TMPDIR/"
cp "$SRC"/Makefile "$TMPDIR/"

cd "$TMPDIR"
make clean 2>&1 || true
make all 2>&1

echo "--- COPYING OUTPUT ---"
cp -v "$TMPDIR"/build/ldtoypad.sprx "$DST/ldtoypad.sprx"
echo "BUILD_EXIT=$?"
