#!/usr/bin/env bash
set -euo pipefail

SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/hello-plugin"
TMPDIR=/tmp/helloworld-build

# Rebuild from source
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"
cp "$SRC/main.c" "$TMPDIR/"
cp "$SRC/Makefile" "$TMPDIR/"
cd "$TMPDIR"
make clean all 2>&1

ELF="$TMPDIR/build/helloworld.elf"
echo "=== SECTIONS ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -S "$ELF" 2>&1

echo ""
echo "=== NM symbols (libent/libstub/sce) ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-nm "$ELF" 2>&1 | grep -E "libent|libstub|sce|module"

echo ""
echo "=== .rodata hex ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .rodata "$ELF" 2>&1

echo ""
echo "=== .lib.ent hex ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.ent "$ELF" 2>&1

echo ""
echo "=== .lib.stub hex ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.stub "$ELF" 2>&1

echo ""
echo "=== .sys_proc_prx_param hex ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .sys_proc_prx_param "$ELF" 2>&1

echo ""
echo "=== RELOCS ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -r "$ELF" 2>&1
