#!/usr/bin/env bash
set -euo pipefail

TMPDIR=/tmp/helloworld-build2
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"

SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/hello-plugin"
cp "$SRC/startup_fixed.s" "$TMPDIR/"
cp "$SRC/Makefile.asm" "$TMPDIR/Makefile"

cd "$TMPDIR"
make clean all 2>&1

echo ""
echo "--- Copying SPRX output ---"
mkdir -p "$SRC/build"
cp -v "$TMPDIR"/build2/helloworld.sprx "$SRC/build/helloworld-asm.sprx" 2>/dev/null || echo "No output"
ls -la "$TMPDIR"/build2/ 2>/dev/null || echo "No build2 dir"
