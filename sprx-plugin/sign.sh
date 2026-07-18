#!/bin/bash
# sign.sh — Sign PRX with oscetool
# Run from WSL: bash sign.sh

PRX="$1"
SPRX="${PRX%.prx}.sprx"
OSCETOOL="$HOME/oscetool-src/oscetool"

cd "$HOME/oscetool-src" || exit 1

"$OSCETOOL" -v \
  -0 SELF \
  -1 TRUE \
  -2 000A \
  -3 1010000001000003 \
  -4 01000002 \
  -5 APP \
  -A 0001000000000000 \
  -6 0004009300000000 \
  -8 4000000000000000000000000000000000000000000000000000000000000002 \
  -e "$PRX" "$SPRX"

echo "=== Signed: $SPRX ==="
ls -la "$SPRX"
