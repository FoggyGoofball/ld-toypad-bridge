#!/usr/bin/env bash
set -euo pipefail

D=/usr/local/ps3dev/ppu/bin

echo "=== ELF HEADER ==="
"$D/powerpc64-ps3-elf-readelf" -h /tmp/ldtoypad-build/build/ldtoypad.elf 2>&1

echo ""
echo "=== SECTIONS ==="
"$D/powerpc64-ps3-elf-readelf" -S /tmp/ldtoypad-build/build/ldtoypad.elf 2>&1

echo ""
echo "=== ENTRY POINT ==="
"$D/powerpc64-ps3-elf-nm" /tmp/ldtoypad-build/build/ldtoypad.elf 2>&1 | grep -E "_start|module_start|module_stop|_stop"

echo ""
echo "=== .lib.ent / .lib.stub / .sys_proc ==="
"$D/powerpc64-ps3-elf-objdump" -s -j .lib.ent /tmp/ldtoypad-build/build/ldtoypad.elf 2>&1
"$D/powerpc64-ps3-elf-objdump" -s -j .lib.stub /tmp/ldtoypad-build/build/ldtoypad.elf 2>&1
"$D/powerpc64-ps3-elf-objdump" -s -j .sys_proc_prx_param /tmp/ldtoypad-build/build/ldtoypad.elf 2>&1
"$D/powerpc64-ps3-elf-objdump" -s -j .rodata.sceModuleInfo /tmp/ldtoypad-build/build/ldtoypad.elf 2>&1

echo ""
echo "=== DISASSEMBLY OF _start ==="
"$D/powerpc64-ps3-elf-objdump" -d /tmp/ldtoypad-build/build/ldtoypad.elf 2>&1 | head -80
