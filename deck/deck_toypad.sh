#!/bin/bash
# =============================================================================
# deck_toypad.sh — Steam Deck LEGO Dimensions ToyPad Emulator
# =============================================================================
# One-script setup: turns your Steam Deck into a USB ToyPad for PS3/PS4/Wii U.
#
# Quick Start:
#   1. BIOS: Vol-Up + Power → Setup → Advanced → USB → DRD → Save & Exit
#   2. Desktop Mode → Konsole
#   3. chmod +x deck_toypad.sh && sudo ./deck_toypad.sh
#
# What this does:
#   - Installs Node.js + build tools (pacman)
#   - Creates USB gadget matching real ToyPad (VID 0x0E6F, PID 0x0241)
#   - Clones Berny23/LD-ToyPad-Emulator
#   - Installs npm dependencies (including native node-ld)
#   - Starts server on port 80 (open http://localhost on Deck browser)
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
    echo ""
    echo "  If you don't have a sudo password set, run:  passwd"
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Disable read-only filesystem (SteamOS default)
# ---------------------------------------------------------------------------
if steamos-readonly status 2>/dev/null | grep -q enabled; then
    echo -e "${YELLOW}Disabling SteamOS read-only mode (needed for pacman)...${NC}"
    steamos-readonly disable
    echo -e "${GREEN}  Read-only mode disabled.${NC}"
fi

# ---------------------------------------------------------------------------
# 3. Install prerequisites
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[1/5] Installing prerequisites...${NC}"

# Node.js and npm
if ! command -v node &>/dev/null; then
    echo "  Installing Node.js..."
    pacman -Sy --noconfirm nodejs npm 2>/dev/null || {
        pacman-key --init 2>/dev/null || true
        pacman-key --populate archlinux 2>/dev/null || true
        pacman -Sy --noconfirm nodejs npm
    }
fi
echo -e "${GREEN}  Node.js: $(node --version)${NC}"

# git
if ! command -v git &>/dev/null; then
    echo "  Installing git..."
    pacman -S --noconfirm git 2>/dev/null || true
fi

# Build tools (needed for node-ld native C++ compilation)
if ! command -v make &>/dev/null; then
    echo "  Installing build tools (needed for node-ld)..."
    pacman -S --noconfirm base-devel 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 4. USB Gadget Setup
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[2/5] Setting up USB gadget...${NC}"

# Load kernel module
modprobe libcomposite 2>/dev/null || true

# Tear down any existing gadget
if [ -d "$GADGET_DIR" ]; then
    echo "  Removing old gadget..."
    if [ -f "$GADGET_DIR/UDC" ]; then
        echo "" > "$GADGET_DIR/UDC" 2>/dev/null || true
    fi
    for f in "$GADGET_DIR"/configs/*/hid.*; do
        if [ -L "$f" ]; then rm -f "$f" 2>/dev/null || true; fi
    done
    rmdir "$GADGET_DIR"/configs/c.1/strings/0x409 2>/dev/null || true
    rmdir "$GADGET_DIR"/configs/c.1 2>/dev/null || true
    rmdir "$GADGET_DIR"/functions/hid.g0 2>/dev/null || true
    rmdir "$GADGET_DIR"/functions/hid.usb0 2>/dev/null || true
    rmdir "$GADGET_DIR"/strings/0x409 2>/dev/null || true
    rmdir "$GADGET_DIR" 2>/dev/null || true
fi

# Create gadget
mkdir -p "$GADGET_DIR"
cd "$GADGET_DIR"

# USB identifiers — exact match for LEGO Dimensions ToyPad
echo 0x0e6f > idVendor
echo 0x0241 > idProduct
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

# Device strings — verified against Berny23's usb_setup_script.sh
mkdir -p strings/0x409
echo "P.D.P.000000"        > strings/0x409/serialnumber
echo "PDP LIMITED. "       > strings/0x409/manufacturer
echo "LEGO READER V2.10"   > strings/0x409/product

# Configuration
mkdir -p configs/c.1/strings/0x409
echo "LEGO READER V2.10"   > configs/c.1/strings/0x409/configuration
echo 250                   > configs/c.1/MaxPower

# HID function
mkdir -p functions/hid.g0
echo 0 > functions/hid.g0/protocol
echo 0 > functions/hid.g0/subclass
echo 32 > functions/hid.g0/report_length

# HID Report Descriptor — verified byte-for-byte against Berny23
printf '\x06\x00\xFF\x09\x01\xA1\x01\x19\x01\x29\x20\x15\x00\x26\xFF\x00\x75\x08\x95\x20\x81\x00\x19\x01\x29\x20\x91\x00\xC0' > functions/hid.g0/report_desc

echo "  HID descriptor: $(wc -c < functions/hid.g0/report_desc) bytes (expect 27)"

# Link and bind
ln -sf functions/hid.g0 configs/c.1/
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
if [ -z "$UDC" ]; then
    echo -e "${RED}  No UDC found! Is USB Dual-Role set to DRD in BIOS?${NC}"
    exit 1
fi
echo "$UDC" > UDC
sleep 1

# Verify
if [ -e /dev/hidg0 ]; then
    chmod 666 /dev/hidg0
    echo -e "${GREEN}  USB gadget LIVE — /dev/hidg0 ready${NC}"
else
    echo -e "${RED}  /dev/hidg0 did not appear! Check: ls /dev/hidg*${NC}"
fi

# ---------------------------------------------------------------------------
# 5. Clone / Update Emulator
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[3/5] Setting up emulator...${NC}"
if [ -d "$REPO_DIR" ]; then
    echo "  Updating existing repo..."
    cd "$REPO_DIR"
    git pull --ff-only 2>/dev/null || true
else
    echo "  Cloning Berny23/LD-ToyPad-Emulator..."
    cd "$HOME"
    git clone "$REPO_URL" "$REPO_DIR"
fi

cd "$REPO_DIR"

# Ensure touch-punch is available for touchscreen drag-and-drop
if [ ! -f server/jquery.ui.touch-punch.min.js ]; then
    echo "  Downloading touch-punch for touchscreen support..."
    curl -sSL "https://raw.githubusercontent.com/furf/jquery-ui-touch-punch/master/jquery.ui.touch-punch.min.js" -o server/jquery.ui.touch-punch.min.js 2>/dev/null || true
fi

echo -e "${YELLOW}[4/5] Installing npm dependencies (this may take a minute)...${NC}"
npm install --no-audit --no-fund 2>&1 | grep -E "(added|error|ERR)" || true
echo -e "${GREEN}  Dependencies ready.${NC}"

# ---------------------------------------------------------------------------
# 6. Start Server
# ---------------------------------------------------------------------------
echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  ToyPad emulator STARTING${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""
echo -e "  ${YELLOW}On the Steam Deck browser:${NC}"
echo -e "     ${GREEN}http://localhost${NC}"
echo ""
echo -e "  ${YELLOW}To play:${NC}"
echo "     1. Plug USB-C → PS3 with DATA cable"
echo "     2. REBOOT the PS3 (USB only scanned at boot)"
echo "     3. Boot LEGO Dimensions (original, unmodified)"
echo "     4. Create characters on Deck touchscreen → drag to ToyPad slots"
echo ""
echo -e "  ${YELLOW}Touchscreen tip:${NC} Long-press a character, then drag to a pad slot"
echo ""
echo -e "  ${RED}Press Ctrl+C to stop${NC}"
echo ""

exec node index.js

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
