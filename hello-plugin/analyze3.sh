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
echo "=== SECTION LIST (grep for key PRX sections) ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -S build/helloworld.elf 2>&1 | grep -E 'sce|lib\.ent|lib\.stub|rodata'

echo ""
echo "=== NM all with 'libent', 'libstub', 'module', 'sce' ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-nm build/helloworld.elf 2>&1 | grep -iE 'libent|libstub|module|sce'

echo ""
echo "=== OBJDUMP all SCE/PRX sections ==="
for sec in rodata.sceReside rodata.sceFNID lib.ent lib.stub rodata.sceModuleInfo; do
  echo "--- $sec ---"
  /usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j "$sec" build/helloworld.elf 2>&1
  echo ""
done
