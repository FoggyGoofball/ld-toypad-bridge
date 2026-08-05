#!/bin/bash
# =============================================================================
# deck-controller.sh — Steam Deck as PS3 Wireless Controller (standalone)
# =============================================================================
# Turns your Steam Deck into a wireless DualShock 3 for ANY PS3 game.
# One-time USB pairing, then Bluetooth forever. No ToyPad, no Dimensions.
#
# USAGE (one-liner):
#   curl -sSL https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/main/deck/deck-controller.sh | sudo bash
#
# WHAT IT DOES:
#   1. Installs deps (gcc, bluez-utils, python-pip, evdev)
#   2. Runs FFS daemon → one-time USB DS3 pairing with PS3
#   3. Connects Bluetooth to PS3 (btmgmt MAC spoof + bluetoothctl)
#   4. Starts evdev→DS3 gamepad daemon (L4+R4 = toggle, L5+R5 = PS button)
#
# PREREQUISITES:
#   - BIOS DRD mode (Vol Up + Power → Setup → Advanced → USB → DRD)
#   - USB-C to USB-A data cable
#   - sudo password set (passwd)
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECK_HOME="/home/deck"
PAIRING_FILE="$DECK_HOME/.config/ld-toypad/ds3-pairing.json"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

echo -e "${GREEN}╔══════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║   Steam Deck → PS3 Wireless Controller      ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════╝${NC}"
echo ""

# Ensure root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Must run as root (sudo).${NC}"; exit 1
fi

# ── 1. System setup ────────────────────────────────────────────
echo -e "${YELLOW}[1/4] System dependencies...${NC}"
sudo steamos-readonly disable 2>/dev/null || true

# pacman keyring (first-boot)
if ! pacman -Q pacman 2>/dev/null | grep -q pacman; then
    pacman-key --init 2>/dev/null || true
    pacman-key --populate archlinux 2>/dev/null || true
fi

# gcc (compile FFS daemon)
command -v gcc &>/dev/null || { echo "  Installing gcc..."; pacman -Sy --noconfirm gcc 2>/dev/null; }

# bluez-utils (btmgmt, bluetoothctl)
command -v btmgmt &>/dev/null || { echo "  Installing bluez-utils..."; pacman -Sy --noconfirm bluez-utils 2>/dev/null; }

# python + pip + evdev
command -v pip &>/dev/null || { echo "  Installing python-pip..."; pacman -Sy --noconfirm python-pip 2>/dev/null; }
python3 -c "import evdev" 2>/dev/null || { echo "  Installing evdev..."; pip install evdev 2>/dev/null || pacman -Sy --noconfirm python-evdev 2>/dev/null; }

echo -e "  ${GREEN}Ready.${NC}"

# ── 2. DS3 USB Pairing (one-time) ──────────────────────────────
echo -e "${YELLOW}[2/4] DS3 Bluetooth Pairing...${NC}"

if [ -f "$PAIRING_FILE" ]; then
    echo -e "  ${GREEN}Already paired:${NC}"
    echo "    Deck BT: $(grep -oP '"deck_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")"
    echo "    PS3 BT:  $(grep -oP '"ps3_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")"
    echo ""
    echo -ne "  Re-pair? [y/N]: "
    read -r confirm
    if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
        echo "  Skipping pairing."
    else
        rm -f "$PAIRING_FILE"
    fi
fi

if [ ! -f "$PAIRING_FILE" ]; then
    # Compile daemon
    echo "  Compiling ds3-pair-daemon..."
    gcc -Wall -O2 -o /tmp/ds3-pair-daemon "$SCRIPT_DIR/ds3-pair-daemon.c" 2>/dev/null || {
        # If running via curl pipe, fetch source
        curl -sSL "https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/main/deck/ds3-pair-daemon.c" -o /tmp/ds3-pair-daemon.c
        gcc -Wall -O2 -o /tmp/ds3-pair-daemon /tmp/ds3-pair-daemon.c
    }

    echo ""
    echo -e "  ${YELLOW}╔══════════════════════════════════════════════╗${NC}"
    echo -e "  ${YELLOW}║  PLUG USB-C CABLE INTO PS3 NOW (30 seconds)  ║${NC}"
    echo -e "  ${YELLOW}╚══════════════════════════════════════════════╝${NC}"
    echo ""

    /tmp/ds3-pair-daemon
    if [ $? -ne 0 ] || [ ! -f "$PAIRING_FILE" ]; then
        echo -e "${RED}Pairing failed. Check USB cable and PS3 power.${NC}"
        exit 1
    fi
    echo -e "  ${GREEN}✓ Paired!${NC}"
fi

# ── 3. Bluetooth Connection ────────────────────────────────────
echo -e "${YELLOW}[3/4] Bluetooth connection to PS3...${NC}"

DECK_MAC=$(grep -oP '"deck_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")
PS3_MAC=$(grep -oP '"ps3_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")

# Spoof MAC
ORIG_MAC=$(hciconfig hci0 2>/dev/null | grep -oP 'BD Address: \K[0-9A-F:]+' || echo "")
if [ "$ORIG_MAC" != "$DECK_MAC" ]; then
    sudo btmgmt public-addr "$DECK_MAC" 2>/dev/null || {
        sudo hciconfig hci0 down
        sudo hciconfig hci0 addr "$DECK_MAC"
        sudo hciconfig hci0 up
    }
    sleep 1
fi

# Load hid-sony
sudo modprobe hid-sony 2>/dev/null || true

# Ensure Bluetooth is running
systemctl is-active --quiet bluetooth || { sudo systemctl start bluetooth; sleep 2; }

# Connect
if ! hcitool con 2>/dev/null | grep -qi "$PS3_MAC"; then
    echo "  Connecting to PS3 (PS3 passively listens for inbound L2CAP)..."
    bluetoothctl <<EOF 2>/dev/null || true
power on
agent on
default-agent
connect $PS3_MAC
EOF
    sleep 2
fi

if hcitool con 2>/dev/null | grep -qi "$PS3_MAC"; then
    echo -e "  ${GREEN}✓ Connected to PS3 as DS3 controller${NC}"
else
    echo -e "  ${YELLOW}⚠ BT connection pending (PS3 may need to be on XMB)${NC}"
fi

# ── 4. Gamepad Daemon ──────────────────────────────────────────
echo -e "${YELLOW}[4/4] Starting gamepad daemon...${NC}"
echo ""
echo -e "  ${GREEN}╔══════════════════════════════════════════════╗${NC}"
echo -e "  ${GREEN}║  Deck is now a PS3 controller!              ║${NC}"
echo -e "  ${GREEN}╠══════════════════════════════════════════════╣${NC}"
echo -e "  ${GREEN}║  L4+R4 = toggle controller / desktop mode   ║${NC}"
echo -e "  ${GREEN}║  L5+R5 = PS button                          ║${NC}"
echo -e "  ${GREEN}║  Trackpads = unused in controller mode      ║${NC}"
echo -e "  ${GREEN}║  Ctrl+C  = exit                              ║${NC}"
echo -e "  ${GREEN}╚══════════════════════════════════════════════╝${NC}"
echo ""

# Fetch gamepad daemon if needed
DAEMON_PATH="$SCRIPT_DIR/ds3-gamepad-daemon.py"
if [ ! -f "$DAEMON_PATH" ]; then
    DAEMON_PATH="/tmp/ds3-gamepad-daemon.py"
    curl -sSL "https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/main/deck/ds3-gamepad-daemon.py" -o "$DAEMON_PATH"
fi

exec python3 "$DAEMON_PATH"
