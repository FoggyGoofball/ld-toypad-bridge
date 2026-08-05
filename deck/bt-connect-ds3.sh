#!/bin/bash
# =============================================================================
# bt-connect-ds3.sh — Connect Steam Deck to PS3 as paired DS3 over Bluetooth
# =============================================================================
# Reads pairing data from ~/.config/ld-toypad/ds3-pairing.json (created by
# ds3-pair-daemon.c), spoofs the Deck's BT MAC to match the paired identity,
# and connects to the PS3 as a Bluetooth HID controller.
#
# Prerequisites:
#   1. Run ds3-pair-daemon once to pair (saves pairing.json)
#   2. Install BlueZ tools: sudo pacman -S bluez bluez-utils
#   3. Load hid-sony: sudo modprobe hid-sony
#
# Usage:
#   sudo ./bt-connect-ds3.sh
# =============================================================================
set -e

PAIRING_FILE="$HOME/.config/ld-toypad/ds3-pairing.json"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

echo -e "${GREEN}=== DS3 Bluetooth Reconnection ===${NC}"

# ── 1. Read pairing data ───────────────────────────────────────
if [ ! -f "$PAIRING_FILE" ]; then
    echo -e "${RED}Pairing data not found at $PAIRING_FILE${NC}"
    echo "Run ds3-pair-daemon first (sudo ./ds3-pair-daemon) to pair with PS3."
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
# Expert confirmed: use btmgmt (modern API), fall back to hciconfig
ORIG_MAC=$(hciconfig hci0 2>/dev/null | grep -oP 'BD Address: \K[0-9A-F:]+' || echo "")
if [ -z "$ORIG_MAC" ]; then
    echo -e "${RED}No Bluetooth adapter found (hci0).${NC}"
    exit 1
fi
echo "  Current BT MAC: $ORIG_MAC"

if [ "$ORIG_MAC" != "$DECK_MAC" ]; then
    echo "  Spoofing BT MAC to: $DECK_MAC"

    # Try btmgmt first (modern BlueZ API — works on all Deck chipsets)
    if sudo btmgmt public-addr "$DECK_MAC" 2>/dev/null; then
        echo "  (using btmgmt)"
    else
        # Fall back to hciconfig (older API, some chipsets reject)
        echo "  btmgmt failed, trying hciconfig..."
        sudo hciconfig hci0 down
        sudo hciconfig hci0 addr "$DECK_MAC"
        sudo hciconfig hci0 up
    fi
    sleep 1

    NEW_MAC=$(hciconfig hci0 2>/dev/null | grep -oP 'BD Address: \K[0-9A-F:]+' || echo "")
    if [ "$NEW_MAC" != "$DECK_MAC" ]; then
        echo -e "${RED}MAC spoof failed. Current: $NEW_MAC${NC}"
        echo "  Tried both btmgmt and hciconfig. Chipset may not support MAC changes."
        exit 1
    fi
    echo -e "  ${GREEN}MAC spoofed successfully${NC}"
fi

# ── 4. Load hid-sony kernel module ─────────────────────────────
if ! lsmod | grep -q hid_sony; then
    echo "  Loading hid-sony kernel module..."
    sudo modprobe hid-sony 2>/dev/null || echo "  (hid-sony may already be built-in)"
fi

# ── 5. Connect to PS3 via Bluetooth ────────────────────────────
echo ""
echo -e "${YELLOW}Attempting Bluetooth connection to PS3...${NC}"
echo "  The PS3 must be powered on and in range."
echo "  Expert confirmed: PS3 does NOT initiate — the controller must."
echo ""

# First, check if already connected
if hcitool con 2>/dev/null | grep -qi "$PS3_MAC"; then
    echo -e "  ${GREEN}Already connected to PS3${NC}"
    exit 0
fi

# Initiate connection to PS3 (PS3 passively listens for inbound L2CAP)
echo "  Initiating L2CAP connection to $PS3_MAC (PSM 0x11/0x13)..."
bluetoothctl <<EOF 2>/dev/null || true
power on
agent on
default-agent
connect $PS3_MAC
EOF

sleep 2

# Verify connection
if hcitool con 2>/dev/null | grep -qi "$PS3_MAC"; then
    echo -e "${GREEN}=== Connected to PS3 as DS3 controller ===${NC}"
    echo ""
    echo "  The Steam Deck is now acting as a wireless DualShock 3."
    echo "  To also use the ToyPad, run: sudo ./run-ui.sh"
    echo ""
    echo "  To restore original BT MAC:"
    echo "    sudo hciconfig hci0 down"
    echo "    sudo hciconfig hci0 addr $ORIG_MAC"
    echo "    sudo hciconfig hci0 up"
else
    echo -e "${RED}Connection failed.${NC}"
    echo ""
    echo "  Troubleshooting:"
    echo "  1. Ensure PS3 is powered on and Bluetooth is enabled"
    echo "  2. Try manually: bluetoothctl"
    echo "     Then: scan on ; pair $PS3_MAC ; connect $PS3_MAC"
    echo "  3. Some PS3 firmware versions require the PS3 to initiate the connection."
    echo "     Try pressing the PS button on a real controller first."
    echo ""
    echo "  Restoring original BT MAC..."
    sudo hciconfig hci0 down
    sudo hciconfig hci0 addr "$ORIG_MAC"
    sudo hciconfig hci0 up
    exit 1
fi
