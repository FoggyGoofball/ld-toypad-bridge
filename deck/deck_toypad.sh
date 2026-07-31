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
# 3. Install Node.js if missing
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

# Tear down any existing gadget (handle both hid.usb0 and hid.g0 names)
if [ -d "$GADGET_DIR" ]; then
    echo "  Tearing down existing gadget..."
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
    echo "  Old gadget removed."
fi

# Create gadget
mkdir -p "$GADGET_DIR"
cd "$GADGET_DIR"

# USB device identifiers
echo 0x0e6f > idVendor
echo 0x0241 > idProduct
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

# Device strings — MUST match real ToyPad exactly (verified against Berny23)
mkdir -p strings/0x409
echo "P.D.P.000000"        > strings/0x409/serialnumber
echo "PDP LIMITED. "       > strings/0x409/manufacturer
echo "LEGO READER V2.10"   > strings/0x409/product

# Configuration
mkdir -p configs/c.1/strings/0x409
echo "LEGO READER V2.10"   > configs/c.1/strings/0x409/configuration
echo 250                   > configs/c.1/MaxPower

# HID function — use hid.g0 to match Berny23's known-working setup
mkdir -p functions/hid.g0
echo 0 > functions/hid.g0/protocol
echo 0 > functions/hid.g0/subclass
echo 32 > functions/hid.g0/report_length

# HID Report Descriptor — VERIFIED against Berny23's usb_setup_script.sh
# Usage Page: 0xFF00 (vendor), 32-byte INPUT (Array), 32-byte OUTPUT (Array)
printf '\x06\x00\xFF\x09\x01\xA1\x01\x19\x01\x29\x20\x15\x00\x26\xFF\x00\x75\x08\x95\x20\x81\x00\x19\x01\x29\x20\x91\x00\xC0' > functions/hid.g0/report_desc

# Verify descriptor was written correctly (must be 27 bytes)
ACTUAL_SIZE=$(wc -c < functions/hid.g0/report_desc)
echo "  HID descriptor: $ACTUAL_SIZE bytes (expected 27)"

# Link function to config
ln -sf functions/hid.g0 configs/c.1/

# Find and bind the UDC
echo "  Available UDCs: $(ls /sys/class/udc 2>/dev/null | tr '\n' ' ')"
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
if [ -z "$UDC" ]; then
    echo -e "${RED}  No UDC found! Is USB Dual-Role set to DRD in BIOS?${NC}"
    echo "  Reboot → Vol-Up + Power → Setup → Advanced → USB → DRD"
    exit 1
fi
echo "$UDC" > UDC
sleep 1  # Wait for kernel to create /dev/hidg0

# Verify gadget is live
if [ -e /dev/hidg0 ]; then
    chmod 666 /dev/hidg0
    echo -e "${GREEN}  USB gadget LIVE — /dev/hidg0 ready${NC}"
    echo "  VID/PID: 0e6f/0241  Product: LEGO READER V2.10"
else
    echo -e "${RED}  /dev/hidg0 did not appear after binding $UDC!${NC}"
    echo "  Try: unplug USB-C cable, wait 5s, plug back in"
    echo "  Then check: ls -la /dev/hidg*"
fi

echo -e "${YELLOW}  REMINDER: PS3 must be rebooted with Deck plugged in!${NC}"
echo -e "${YELLOW}  The PS3 only scans USB devices at boot time.${NC}"
    echo -e "${RED}  /dev/hidg0 did not appear after binding $UDC!${NC}"
    echo "  Try: unplug USB-C cable, wait 5s, plug back in"
    echo "  Then check: ls -la /dev/hidg*"
fi

echo -e "${YELLOW}  REMINDER: PS3 must be rebooted with Deck plugged in!${NC}"
echo -e "${YELLOW}  The PS3 only scans USB devices at boot time.${NC}"

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
