#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./build.sh [source_dir]
#
# Optional env:
#   FIX_PSL1GHT=1                Apply sys/usbd.h patch before build
#   RESTORE_MAIN_FROM=/path/main.c  Restore main.c before source fixes

SRC_DIR="${1:-$(pwd)}"

if [[ ! -d "$SRC_DIR" ]]; then
	echo "[build] Source directory not found: $SRC_DIR" >&2
	exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [[ "${FIX_PSL1GHT:-0}" == "1" ]]; then
	"$SCRIPT_DIR/fix_psl1ght.sh"
fi

RESTORE_MAIN_FROM="${RESTORE_MAIN_FROM:-}" "$SCRIPT_DIR/fix_sources.sh" "$SRC_DIR"

make -C "$SRC_DIR" clean >/dev/null 2>&1 || true
make -C "$SRC_DIR" all

