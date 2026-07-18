#!/usr/bin/env bash
set -euo pipefail

# Build LD-ToyPad Bridge SPRX using Sony SDK
# Copies sources to /tmp to avoid Windows pathspace issues

SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin"
TMPDIR=/tmp/ldtoypad-build
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"

cp "$SRC/main.c" "$SRC/ldd_driver.c" "$SRC/network.c" "$SRC/toypad_state.c" \
   "$SRC/debug.c" "$SRC/compat.c" "$TMPDIR/"
cp "$SRC/ldd_driver.h" "$SRC/network.h" "$SRC/debug.h" "$SRC/toypad_state.h" \
   "$SRC/syscall.h" "$TMPDIR/"

CELL_SDK="/mnt/c/usr/local/cell"
CC="$CELL_SDK/host-win32/ppu/ppu-lv2/bin/gcc.exe"
LD="$CC"

CFLAGS="-mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs -I$CELL_SDK/target/ppu/include"
LDFLAGS="-mprx -nodefaultlibs -llv2_stub -lfs_stub -lnet_stub -lsysmodule_stub -lusbd_stub -lc_stub"

cd "$TMPDIR"
mkdir -p obj build

echo "=== Compiling ==="
for f in main.c ldd_driver.c network.c toypad_state.c debug.c compat.c; do
  echo "  CC    $f"
  $CC $CFLAGS -c "$f" -o "obj/${f%.c}.o" 2>&1
done

echo "=== Linking ==="
$LD obj/*.o $LDFLAGS -o "build/ldtoypad.prx" 2>&1
ls -la "build/ldtoypad.prx"

echo "=== Signing with make_self ==="
PATH="/usr/local/ps3dev/bin:$PATH"
make_self "build/ldtoypad.prx" "build/ldtoypad.sprx" 2>&1
ls -la "build/ldtoypad.sprx"

echo "=== Copying output ==="
mkdir -p "$SRC/build"
cp -v "build/ldtoypad.prx" "$SRC/build/ldtoypad.prx"
cp -v "build/ldtoypad.sprx" "$SRC/build/ldtoypad.sprx"
echo "=== Build complete ==="
