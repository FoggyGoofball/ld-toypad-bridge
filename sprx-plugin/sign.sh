#!/bin/bash
# sign.sh — Copy PRX from Windows path to WSL tmp, sign with oscetool, copy SPRX back
# Called from Makefile via: wsl.exe bash sign.sh <target>

TARGET="${1:-ldtoypad}"
SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin/build/${TARGET}.prx"
DST="/tmp/${TARGET}.sprx"
OUT="/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin/build/${TARGET}.sprx"
OSCETOOL="$HOME/oscetool-src/oscetool"

FLAGS="-0 SELF -1 TRUE -2 000A -3 1010000001000003 -4 01000002"
FLAGS="$FLAGS -A 0001000000000000 -5 APP"
FLAGS="$FLAGS -8 4000000000000000000000000000000000000000000000000000000000000002"
FLAGS="$FLAGS -6 0004009300000000"

cp "$SRC" /tmp/${TARGET}.prx || exit 1
cd "$HOME/oscetool-src" || exit 1
"$OSCETOOL" -v $FLAGS -e /tmp/${TARGET}.prx "$DST" || exit 1
cp "$DST" "$OUT" || exit 1
ls -la "$DST" "$OUT"
echo "=== Signed: ${TARGET}.sprx ==="
