#!/usr/bin/env bash
set -euo pipefail

USBD_H="${1:-/usr/local/ps3dev/psl1ght/ppu/include/sys/usbd.h}"

if [[ ! -f "$USBD_H" ]]; then
	echo "[fix_psl1ght] File not found: $USBD_H" >&2
	exit 1
fi

cp -f "$USBD_H" "${USBD_H}.bak"

# Known bad field names in some PSL1GHT snapshots.
sed -i 's/lv2syscall4(534, handle, unk1, descriptor, descSize);/lv2syscall4(534, handle, deviceNumber, descriptor, descSize);/g' "$USBD_H"
sed -i 's/lv2syscall3(535, unk1, endpoint, transfer);/lv2syscall3(535, deviceNumber, endpoint, transfer);/g' "$USBD_H"

echo "[fix_psl1ght] Patched: $USBD_H"
echo "[fix_psl1ght] Backup : ${USBD_H}.bak"
