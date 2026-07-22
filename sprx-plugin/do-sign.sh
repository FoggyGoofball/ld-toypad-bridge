#!/bin/bash
# Sign the PRX using oscetool
SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin/build/ldtoypad.prx"
DST="/tmp/ldtoypad.sprx"
OUT="/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin/build/ldtoypad.sprx"

echo "Copying PRX..."
cp "$SRC" /tmp/ldtoypad.prx || exit 1
ls -la /tmp/ldtoypad.prx

echo "Signing..."
cd $HOME/oscetool-src || exit 1
./oscetool -v -0 SELF -1 TRUE -2 000A -3 1010000001000003 -4 01000002 -A 0001000000000000 -5 ISLAND -8 4000000000000000000000000000000000000000000000000000000000000002 -6 0004009300000000 -e /tmp/ldtoypad.prx "$DST" || exit 1

echo "Copying result..."
cp "$DST" "$OUT" || exit 1
ls -la "$DST" "$OUT"
echo '=== Signed successfully ==='
