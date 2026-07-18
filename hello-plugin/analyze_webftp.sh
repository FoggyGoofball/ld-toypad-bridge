#!/usr/bin/env bash
set -euo pipefail

# Copy webftp.sprx from Windows desktop to WSL
cp /mnt/c/Users/Admin/Desktop/webftp_server.sprx /tmp/webftp.sprx
cd /tmp

# Decrypt the SPRX to an ELF
/usr/local/ps3dev/bin/fself webftp.sprx webftp.elf 2>&1
echo "=== DSELF OK ==="

# Show all sections
echo "=== SECTIONS ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -S webftp.elf 2>&1

echo ""
echo "=== NM SYMBOLS (all) ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-nm webftp.elf 2>&1

echo ""
echo "=== .sys_proc_prx_param raw ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .sys_proc_prx_param webftp.elf 2>&1

echo ""
echo "=== .rodata raw ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .rodata webftp.elf 2>&1

echo ""
echo "=== .lib.ent raw ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.ent webftp.elf 2>&1

echo ""
echo "=== .lib.stub raw ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-objdump -s -j .lib.stub webftp.elf 2>&1

echo ""
echo "=== HEADER ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -h webftp.elf 2>&1

echo ""
echo "=== DYNAMIC ==="
/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-readelf -d webftp.elf 2>&1
