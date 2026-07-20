#!/usr/bin/env bash
# Build script for LD-ToyPad Bridge SPRX (Sony SDK)
# Copies sources to /tmp, builds with WSL-accessible paths.
set -euo pipefail

SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin"
TMPDIR=/tmp/ldtoypad-build
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"

# Copy all C sources and headers
cp "$SRC"/main.c "$SRC"/network.c "$SRC"/toypad_state.c \
   "$SRC"/debug.c "$SRC"/compat.c "$SRC"/hook.c "$SRC"/usb_hooks.c "$TMPDIR/"
cp "$SRC"/network.h "$SRC"/debug.h "$SRC"/toypad_state.h \
   "$SRC"/hook.h "$SRC"/usb_hooks.h "$TMPDIR/"

# Write a WSL-native Makefile
CELL_SDK="/mnt/c/usr/local/cell"
CROSS_PREFIX="$CELL_SDK/host-win32/ppu/ppu-lv2"

CC="$CROSS_PREFIX/bin/gcc.exe"
LD="$CROSS_PREFIX/bin/gcc.exe"
SCETOOL="wsl /usr/local/ps3dev/bin/make_self"

TARGET="ldtoypad"
BUILDDIR="build"
OBJDIR="obj"

C_SRCS="main.c compat.c network.c debug.c toypad_state.c hook.c usb_hooks.c"
C_OBJS=""
for f in $C_SRCS; do
  C_OBJS="$C_OBJS $OBJDIR/${f%.c}.o"
done

INCLUDES="-I$CELL_SDK/target/ppu/include"
CFLAGS="-mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs $INCLUDES"
LDFLAGS="-mprx -nodefaultlibs -llv2_stub -lfs_stub -lnet_stub"

cd "$TMPDIR"
mkdir -p "$OBJDIR" "$BUILDDIR"

echo "=== Compiling ==="
for f in $C_SRCS; do
  o="$OBJDIR/${f%.c}.o"
  echo "  CC    $f"
  $CC $CFLAGS -c "$f" -o "$o"
done

echo "=== Linking $TARGET.prx ==="
$LD $C_OBJS $LDFLAGS -o "$BUILDDIR/$TARGET.prx"
ls -la "$BUILDDIR/$TARGET.prx"

echo "=== Signing with make_self ==="
make_self "$BUILDDIR/$TARGET.prx" "$BUILDDIR/$TARGET.sprx"
ls -la "$BUILDDIR/$TARGET.sprx"

echo "=== Copying output ==="
mkdir -p "$SRC/build"
cp -v "$BUILDDIR/$TARGET.prx" "$SRC/build/$TARGET.prx"
cp -v "$BUILDDIR/$TARGET.sprx" "$SRC/build/$TARGET.sprx"
echo "=== Build complete ==="
