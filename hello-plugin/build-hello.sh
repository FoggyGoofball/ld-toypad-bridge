#!/usr/bin/env bash
set -euo pipefail

SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/hello-plugin"
TMPDIR=/tmp/helloworld-build
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"
cp "$SRC/main.c" "$TMPDIR/"
cp "$SRC/Makefile" "$TMPDIR/"
cd "$TMPDIR"
make clean all 2>&1
echo "--- COPYING OUTPUT ---"
mkdir -p "$SRC/build"
cp -v "$TMPDIR"/build/helloworld.sprx "$SRC/build/helloworld.sprx" 2>/dev/null || echo "No sprx produced"
ls -la "$TMPDIR"/build/ 2>/dev/null || echo "No build dir"
