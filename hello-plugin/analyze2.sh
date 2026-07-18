#!/usr/bin/env bash
set -euo pipefail

TMPDIR=/tmp/helloworld-build
cd "$TMPDIR"

echo "=== OBJDUMP .rodata ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .rodata build/helloworld.elf 2>&1

echo ""
echo "=== OBJDUMP .rodata.sceReside ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .rodata.sceReside build/helloworld.elf 2>&1

echo ""
echo "=== OBJDUMP .lib.ent (BINARY) ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.ent build/helloworld.elf 2>&1

echo ""
echo "=== OBJDUMP .lib.stub (BINARY) ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.stub build/helloworld.elf 2>&1

echo ""
echo "=== NM all symbols containing 'libent' or 'libstub' or 'module' or 'sce' ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-nm build/helloworld.elf 2>&1 | grep -iE 'libent|libstub|module|sce'

echo ""
echo "=== CHECK: does our inline asm section name survive? ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -S build/helloworld.elf 2>&1 | grep -i 'sceModule\|sceReside\|sceFNID'
