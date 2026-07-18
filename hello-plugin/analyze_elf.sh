#!/usr/bin/env bash
set -euo pipefail

TMPDIR=/tmp/helloworld-build
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"

SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/hello-plugin"
cp "$SRC/main.c" "$TMPDIR/"
cp "$SRC/Makefile" "$TMPDIR/"
cd "$TMPDIR"
make clean all 2>&1

echo ""
echo "=== NM SYMBOL TABLE ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-nm build/helloworld.elf 2>&1 | sort

echo ""
echo "=== READELF SECTIONS ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -S build/helloworld.elf 2>&1

echo ""
echo "=== OBJDUMP .rodata.sceModuleInfo ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .rodata.sceModuleInfo build/helloworld.elf 2>&1

echo ""
echo "=== OBJDUMP .lib.ent ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.ent build/helloworld.elf 2>&1

echo ""
echo "=== OBJDUMP .lib.stub ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.stub build/helloworld.elf 2>&1

echo ""
echo "=== OBJDUMP .rela.text ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .rela.text build/helloworld.elf 2>&1 || echo "(no .rela.text section)"

echo ""
echo "=== OBJDUMP .prx_relocs ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .prx_relocs build/helloworld.elf 2>&1 || echo "(no .prx_relocs section)"
