#!/bin/bash
# =============================================================================
# deck_toypad.sh — Steam Deck LEGO Dimensions ToyPad Emulator
# =============================================================================
# One-script setup: turns your Steam Deck into a USB ToyPad for PS3/PS4/Wii U.
#
# Usage:
#   1. Set USB Dual-Role to DRD in BIOS first (Vol-Up + Power → Setup → Advanced → USB)
#   2. Boot Desktop Mode, open Konsole
#   3. Run:  chmod +x deck_toypad.sh && sudo ./deck_toypad.sh
#
# What this does:
#   - Installs Node.js if missing (pacman)
#   - Creates USB gadget with LEGO ToyPad VID/PID and HID descriptor
#   - Clones Berny23's LD-ToyPad-Emulator
#   - Installs npm dependencies
#   - Starts the emulator server
#   - Opens the web UI
# =============================================================================

set -e

GADGET_DIR=/sys/kernel/config/usb_gadget/g1
REPO_URL="https://github.com/Berny23/LD-ToyPad-Emulator.git"
REPO_DIR="$HOME/LD-ToyPad-Emulator"
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  LEGO Dimensions ToyPad — Steam Deck Setup ${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""

# ---------------------------------------------------------------------------
# 1. Ensure we're running as root
# ---------------------------------------------------------------------------
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}This script must run as root (sudo).${NC}"
    echo "  sudo ./deck_toypad.sh"
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Install Node.js if missing
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[1/5] Checking Node.js...${NC}"
if ! command -v node &>/dev/null; then
    echo "  Node.js not found. Installing via pacman..."
    if ! command -v pacman &>/dev/null; then
        echo -e "${RED}pacman not found — are you on SteamOS/Arch?${NC}"
        exit 1
    fi
    pacman -Sy --noconfirm nodejs npm 2>/dev/null || {
        echo "  pacman -Sy failed, trying with key init..."
        pacman-key --init 2>/dev/null || true
        pacman-key --populate archlinux 2>/dev/null || true
        pacman -Sy --noconfirm nodejs npm
    }
    echo -e "${GREEN}  Node.js installed.${NC}"
else
    echo -e "${GREEN}  Node.js found: $(node --version)${NC}"
fi

if ! command -v git &>/dev/null; then
    echo "  Installing git..."
    pacman -S --noconfirm git 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 3. USB Gadget Setup
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[2/5] Setting up USB gadget...${NC}"

# Load kernel module
modprobe libcomposite 2>/dev/null || true

# Tear down any existing gadget
if [ -d "$GADGET_DIR" ]; then
    echo "  Tearing down existing gadget..."
    if [ -f "$GADGET_DIR/UDC" ]; then
        echo "" > "$GADGET_DIR/UDC" 2>/dev/null || true
    fi
    for f in "$GADGET_DIR"/configs/*/hid.usb0; do
        if [ -L "$f" ]; then
            rm -f "$f" 2>/dev/null || true
        fi
    done
    rmdir "$GADGET_DIR"/configs/c.1/strings/0x409 2>/dev/null || true
    rmdir "$GADGET_DIR"/configs/c.1 2>/dev/null || true
    rmdir "$GADGET_DIR"/functions/hid.usb0 2>/dev/null || true
    rmdir "$GADGET_DIR"/strings/0x409 2>/dev/null || true
    rmdir "$GADGET_DIR" 2>/dev/null || true
fi

# Create gadget
mkdir -p "$GADGET_DIR"
cd "$GADGET_DIR"

# USB device identifiers — MUST match the real LEGO ToyPad
echo 0x0e6f > idVendor   # PDP/Logic3
echo 0x0241 > idProduct  # LEGO Dimensions Toy Pad
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

# Device strings
mkdir -p strings/0x409
echo "0000000001"          > strings/0x409/serialnumber
echo "PDP"                 > strings/0x409/manufacturer
echo "LEGO ToyPad"         > strings/0x409/product

# Configuration
mkdir -p configs/c.1/strings/0x409
echo "Config 1"            > configs/c.1/strings/0x409/configuration
echo 250                   > configs/c.1/MaxPower

# HID function
mkdir -p functions/hid.usb0
echo 0 > functions/hid.usb0/protocol
echo 0 > functions/hid.usb0/subclass
echo 80 > functions/hid.usb0/report_length

# HID Report Descriptor — critical for PS3 to recognize the ToyPad
# Usage Page: FF00 (vendor), 80-byte input, 8-byte output
echo -ne "\x06\x00\xFF\x09\x01\xA1\x01\x09\x02\x15\x00\x26\xFF\x00\x75\x08\x95\x50\x81\x02\x09\x03\x75\x08\x95\x08\x91\x02\xC0" > functions/hid.usb0/report_desc

# Link function to config and bind
ln -sf functions/hid.usb0 configs/c.1/

# Find and bind the UDC (USB Device Controller)
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
if [ -z "$UDC" ]; then
    echo -e "${RED}  No UDC found! Is USB Dual-Role set to DRD in BIOS?${NC}"
    echo "  Reboot → Vol-Up + Power → Setup → Advanced → USB → DRD"
    exit 1
fi
echo "$UDC" > UDC
echo -e "${GREEN}  USB gadget bound to $UDC${NC}"

# Give user access
chmod 666 /dev/hidg0 2>/dev/null || true

# ---------------------------------------------------------------------------
# 4. Clone / Update Emulator
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[3/5] Setting up emulator...${NC}"
if [ -d "$REPO_DIR" ]; then
    echo "  Repo exists, pulling latest..."
    cd "$REPO_DIR"
    git pull --ff-only 2>/dev/null || true
else
    echo "  Cloning Berny23/LD-ToyPad-Emulator..."
    cd "$HOME"
    git clone "$REPO_URL" "$REPO_DIR"
fi

cd "$REPO_DIR"
npm install --no-audit --no-fund 2>&1 | tail -1
echo -e "${GREEN}  Dependencies installed.${NC}"

# ---------------------------------------------------------------------------
# 5. Start Server
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[4/5] Starting ToyPad emulator...${NC}"
echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  ToyPad is RUNNING!${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""
echo -e "  ${YELLOW}Web UI:${NC}  http://localhost"
echo -e "  ${YELLOW}Status:${NC}  Listening on /dev/hidg0"
echo ""
echo -e "  ${YELLOW}Next:${NC}"
echo "    1. Plug USB-C → USB-A cable from Deck to PS3"
echo "    2. Boot LEGO Dimensions on PS3 (original unmodified ISO)"
echo "    3. Place figures on the touchscreen"
echo ""
echo -e "  ${RED}Press Ctrl+C to stop${NC}"
echo ""

exec node index.js
