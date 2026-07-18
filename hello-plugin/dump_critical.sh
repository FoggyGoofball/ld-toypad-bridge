#!/usr/bin/env bash
set -euo pipefail

SRC="/mnt/c/Users/Admin/source/repos/dimensions plugin/hello-plugin"
TMPDIR=/tmp/helloworld-build
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"
cp "$SRC/main.c" "$TMPDIR/" 2>/dev/null
cp "$SRC/Makefile" "$TMPDIR/" 2>/dev/null
cd "$TMPDIR"
make clean all 2>&1 > /dev/null
ELF="$TMPDIR/build/helloworld.elf"
if [ ! -f "$ELF" ]; then
    echo "Error: ELF not found"
    exit 1
fi

ELF="$TMPDIR/build/helloworld.elf"

echo "=== NM symbols (grep) ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-nm "$ELF" 2>&1 | grep -E "libent|libstub|sce|module|__"

echo ""
echo "=== .rodata HEX ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .rodata "$ELF" 2>&1

echo ""
echo "=== .lib.ent HEX ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.ent "$ELF" 2>&1

echo ""
echo "=== .lib.stub HEX ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.stub "$ELF" 2>&1

echo ""
echo "=== .sys_proc_prx_param HEX ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .sys_proc_prx_param "$ELF" 2>&1

echo ""
echo "=== .rodata.sceFNID HEX ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .rodata.sceFNID "$ELF" 2>&1

echo ""
echo "=== .rodata.sceReside HEX ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .rodata.sceReside "$ELF" 2>&1

echo ""
echo "=== .data.sceFStub HEX ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .data.sceFStub "$ELF" 2>&1

echo ""
echo "=== RELOCS for .rodata ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -r "$ELF" 2>&1 | grep -A2 "\.rodata\]"

echo ""
echo "=== RELOCS for .sys_proc_prx_param ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -r "$ELF" 2>&1 | grep -A2 "sys_proc"
