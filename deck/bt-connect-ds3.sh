#!/bin/bash
# =============================================================================
# bt-connect-ds3.sh — Spoof BT MAC for DS3 pairing (L2CAP handled by daemon)
# =============================================================================
# Reads ds3-pairing.json, spoofs Deck's BT MAC, ensures adapter is powered on.
# The Python daemon (ds3-gamepad-daemon.py) handles the actual L2CAP connection
# and HID report streaming. bluetoothctl is NOT used — PS3 is a passive
# listener; the controller must initiate raw L2CAP on PSM 0x11/0x13.
#
# Prerequisites:
#   1. Run ds3-pair-daemon once to pair (saves pairing.json)
#   2. Install BlueZ: sudo pacman -S bluez bluez-utils
#
# Usage:
#   sudo ./bt-connect-ds3.sh
# =============================================================================
set -e

PAIRING_FILE="$HOME/.config/ld-toypad/ds3-pairing.json"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

echo -e "${GREEN}=== DS3 Bluetooth MAC Spoof ===${NC}"

# ── 1. Read pairing data ───────────────────────────────────────
if [ ! -f "$PAIRING_FILE" ]; then
    echo -e "${RED}Pairing data not found at $PAIRING_FILE${NC}"
    echo "Run ds3-pair-daemon first (or use option 3 in ldtoypad.sh) to pair with PS3."
    exit 1
fi

DECK_MAC=$(grep -oP '"deck_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")
PS3_MAC=$(grep -oP '"ps3_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")

if [ -z "$DECK_MAC" ] || [ -z "$PS3_MAC" ]; then
    echo -e "${RED}Invalid pairing data in $PAIRING_FILE${NC}"
    exit 1
fi

echo "  Deck BT MAC (spoofed): $DECK_MAC"
echo "  PS3 BT MAC (target):   $PS3_MAC"

# ── 2. Ensure BlueZ is running ─────────────────────────────────
if ! systemctl is-active --quiet bluetooth 2>/dev/null; then
    echo -e "${YELLOW}Starting Bluetooth...${NC}"
    sudo systemctl start bluetooth
    sleep 2
fi

# ── 3. Spoof Bluetooth MAC ─────────────────────────────────────
ORIG_MAC=$(hciconfig hci0 2>/dev/null | grep -oP 'BD Address: \K[0-9A-F:]+' || echo "")
if [ -z "$ORIG_MAC" ]; then
    echo -e "${RED}No Bluetooth adapter found (hci0).${NC}"
    exit 1
fi
echo "  Current BT MAC: $ORIG_MAC"

if [ "$ORIG_MAC" != "$DECK_MAC" ]; then
    echo "  Spoofing BT MAC to: $DECK_MAC"
    # Expert confirmed: btmgmt is the modern API (works on all Deck chipsets)
    if sudo btmgmt public-addr "$DECK_MAC" 2>/dev/null; then
        echo "  (using btmgmt)"
    else
        echo "  btmgmt failed, trying hciconfig..."
        sudo hciconfig hci0 down
        sudo hciconfig hci0 addr "$DECK_MAC"
        sudo hciconfig hci0 up
    fi
    sleep 1

    NEW_MAC=$(hciconfig hci0 2>/dev/null | grep -oP 'BD Address: \K[0-9A-F:]+' || echo "")
    if [ "$NEW_MAC" != "$DECK_MAC" ]; then
        echo -e "${RED}MAC spoof failed. Current: $NEW_MAC${NC}"
        exit 1
    fi
    echo -e "  ${GREEN}MAC spoofed successfully${NC}"
fi

# ── 4. Load hid-sony kernel module ─────────────────────────────
sudo modprobe hid-sony 2>/dev/null || true

# ── 5. Done — daemon handles L2CAP ─────────────────────────────
echo ""
echo -e "${GREEN}=== Bluetooth ready ===${NC}"
echo "  MAC spoofed: $DECK_MAC"
echo "  PS3 target:  $PS3_MAC"
echo ""
echo "  Next: run ds3-gamepad-daemon.py to connect L2CAP and start streaming."
echo "  (The daemon opens raw L2CAP sockets on PSM 0x11 + 0x13 — no bluetoothctl needed.)"
echo ""
echo "  To restore original BT MAC:"
echo "    sudo btmgmt public-addr $ORIG_MAC 2>/dev/null || (sudo hciconfig hci0 down && sudo hciconfig hci0 addr $ORIG_MAC && sudo hciconfig hci0 up)"
