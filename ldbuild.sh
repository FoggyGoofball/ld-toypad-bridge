#!/usr/bin/env bash
set -euo pipefail

TMPDIR=/tmp/ldtoypad-build
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"

SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin"
DST="/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin/build"

cp "$SRC"/*.c "$TMPDIR/"
cp "$SRC"/*.h "$TMPDIR/"
cp "$SRC"/*.S "$TMPDIR/"
cp "$SRC"/Makefile "$TMPDIR/"

cd "$TMPDIR"
make clean 2>&1 || true
make all 2>&1

echo "--- COPYING OUTPUT ---"
cp -v "$TMPDIR"/build/ldtoypad.sprx "$DST/ldtoypad.sprx"
echo "BUILD_EXIT=$?"
