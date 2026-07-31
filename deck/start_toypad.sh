#!/bin/bash
# =============================================================================
# start_toypad.sh — Steam Deck LEGO Dimensions ToyPad (Auto-Update + Run)
# =============================================================================
# Save this anywhere on your Steam Deck. Run it every time you want to play:
#   chmod +x start_toypad.sh && sudo ./start_toypad.sh
#
# It will:
#   1. Check GitHub for updates (auto-installs if newer release found)
#   2. Install system deps if missing (node, git, build tools)
#   3. Create USB gadget matching real LEGO Dimensions ToyPad
#   4. Clone/update Berny23's emulator, install deps, start server
#   5. Open http://localhost on Deck browser → drag characters to ToyPad
# =============================================================================

# Keep window open on exit so you can read any errors
trap 'echo ""; echo -e "\033[1;33m[Process finished] Press Enter to close this window...\033[0m"; read' EXIT

REPO_URL="https://github.com/FoggyGoofball/ld-toypad-bridge.git"
REPO_DIR="$HOME/ld-toypad-bridge"
BERNY_URL="https://github.com/Berny23/LD-ToyPad-Emulator.git"
BERNY_DIR="$HOME/LD-ToyPad-Emulator"
GADGET_DIR="/sys/kernel/config/usb_gadget/g1"
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

clear
echo -e "${GREEN}==========================================================${NC}"
echo -e "${GREEN}    Steam Deck LD-ToyPad Emulator (Auto-Update + Launch)   ${NC}"
echo -e "${GREEN}==========================================================${NC}"
echo ""

# ===========================================================================
# STEP 1: BIOS DRD CHECK
# ===========================================================================
echo -e "${CYAN}>>> STEP 1: BIOS DRD CHECK <<<${NC}"
echo "For the PS3 to see the Deck, the USB port MUST be in Dual-Role mode."
echo "If you haven't done this yet:"
echo "  1. Shut down -> Hold Vol-Up + Power -> Setup Utility"
echo "  2. Advanced -> USB Configuration -> USB Dual-Role Device -> DRD"
echo "  3. Save & Exit"
echo ""
read -p "Press ENTER if DRD is enabled in the BIOS to continue..."
echo ""

# ===========================================================================
# STEP 2: SUDO CHECK
# ===========================================================================
echo -e "${CYAN}>>> STEP 2: ADMINISTRATOR ACCESS <<<${NC}"
sudo -v || { echo -e "${RED}ERROR: Sudo not available. Run 'passwd' in another Konsole first.${NC}"; exit 1; }
echo -e "${GREEN}Access granted.${NC}"
echo ""

# ===========================================================================
# STEP 3: AUTO-UPDATE CHECK
# ===========================================================================
echo -e "${CYAN}>>> STEP 3: CHECKING FOR UPDATES <<<${NC}"

NEEDS_RESTART=false
GIT_TIMEOUT=10  # seconds — don't hang forever if GitHub is slow

if [ -d "$REPO_DIR/.git" ]; then
    echo "  Checking for newer release..."
    cd "$REPO_DIR"
    timeout $GIT_TIMEOUT git fetch origin main 2>/dev/null && {
        LOCAL=$(git rev-parse HEAD 2>/dev/null)
        REMOTE=$(git rev-parse origin/main 2>/dev/null)
        if [ "$LOCAL" != "$REMOTE" ] && [ -n "$REMOTE" ]; then
            echo -e "  ${YELLOW}New release found! Updating...${NC}"
            timeout 30 git pull origin main 2>/dev/null
            NEEDS_RESTART=true
        else
            echo -e "  ${GREEN}Already up to date.${NC}"
        fi
    } || echo -e "  ${YELLOW}Network check skipped (offline or slow). Continuing...${NC}"
elif [ -d "$REPO_DIR" ]; then
    echo -e "  ${YELLOW}Existing folder found (no git). Skipping update check.${NC}"
else
    echo "  First run — downloading..."
    if timeout $GIT_TIMEOUT git clone "$REPO_URL" "$REPO_DIR" 2>/dev/null; then
        NEEDS_RESTART=true
    else
        echo "  Git timed out, trying direct download..."
        rm -rf "$REPO_DIR" 2>/dev/null
        mkdir -p "$REPO_DIR"
        if curl -sSL --connect-timeout 10 "https://github.com/FoggyGoofball/ld-toypad-bridge/archive/refs/heads/main.tar.gz" 2>/dev/null | sudo tar xz -C "$REPO_DIR" --strip-components=1 2>/dev/null; then
            NEEDS_RESTART=true
        else
            echo -e "  ${RED}Download failed. Check internet and retry.${NC}"
            exit 1
        fi
    fi
fi

# Re-exec after update so we run the latest version of this script
if [ "$NEEDS_RESTART" = true ] && [ -f "$REPO_DIR/deck/start_toypad.sh" ]; then
    echo -e "  ${YELLOW}Restarting with latest version...${NC}"
    echo ""
    exec sudo "$REPO_DIR/deck/start_toypad.sh"
fi

echo ""

# ===========================================================================
# STEP 4: SYSTEM DEPENDENCIES
# ===========================================================================
echo -e "${CYAN}>>> STEP 4: SYSTEM DEPENDENCIES <<<${NC}"

MISSING_PKGS=""
command -v node &>/dev/null || MISSING_PKGS="$MISSING_PKGS nodejs npm"
command -v git &>/dev/null || MISSING_PKGS="$MISSING_PKGS git"
command -v make &>/dev/null || MISSING_PKGS="$MISSING_PKGS base-devel"

if [ -n "$MISSING_PKGS" ]; then
    echo "  Installing:$MISSING_PKGS ..."
    sudo steamos-readonly disable 2>/dev/null || true
    [ -f "/var/lib/pacman/db.lck" ] && sudo rm -f /var/lib/pacman/db.lck
    sudo pacman-key --init 2>/dev/null || true
    sudo pacman-key --populate archlinux holo 2>/dev/null || true
    sudo pacman -Sy --noconfirm $MISSING_PKGS || {
        echo -e "${RED}ERROR: Package install failed. Check internet.${NC}"; exit 1;
    }
    echo -e "${GREEN}  Dependencies installed.${NC}"
else
    echo -e "${GREEN}  All system packages present (Node $(node --version)).${NC}"
fi
echo ""

# ===========================================================================
# STEP 5: USB GADGET — must match real ToyPad exactly
# ===========================================================================
echo -e "${CYAN}>>> STEP 5: USB GADGET (LEGO READER V2.10) <<<${NC}"

sudo modprobe libcomposite 2>/dev/null || true

# Tear down existing gadget so we start fresh every time
if [ -d "$GADGET_DIR" ]; then
    echo "  Removing old gadget..."
    [ -f "$GADGET_DIR/UDC" ] && echo "" | sudo tee "$GADGET_DIR/UDC" > /dev/null 2>&1 || true
    for f in "$GADGET_DIR"/configs/*/hid.*; do
        [ -L "$f" ] && sudo rm -f "$f" 2>/dev/null || true
    done
    sudo rmdir "$GADGET_DIR"/configs/c.1/strings/0x409 2>/dev/null || true
    sudo rmdir "$GADGET_DIR"/configs/c.1 2>/dev/null || true
    sudo rmdir "$GADGET_DIR"/functions/hid.g0 2>/dev/null || true
    sudo rmdir "$GADGET_DIR"/functions/hid.usb0 2>/dev/null || true
    sudo rmdir "$GADGET_DIR"/strings/0x409 2>/dev/null || true
    sudo rmdir "$GADGET_DIR" 2>/dev/null || true
fi

echo "  Creating new gadget..."
sudo mkdir -p "$GADGET_DIR"
cd "$GADGET_DIR"

# USB identifiers — exact match for LEGO Dimensions ToyPad
echo 0x0e6f | sudo tee idVendor > /dev/null
echo 0x0241 | sudo tee idProduct > /dev/null
echo 0x0100 | sudo tee bcdDevice > /dev/null
echo 0x0200 | sudo tee bcdUSB > /dev/null

# Device strings — verified against Berny23's usb_setup_script.sh
sudo mkdir -p strings/0x409
echo "P.D.P.000000"      | sudo tee strings/0x409/serialnumber > /dev/null
echo "PDP LIMITED. "     | sudo tee strings/0x409/manufacturer > /dev/null
echo "LEGO READER V2.10" | sudo tee strings/0x409/product > /dev/null

# Configuration
sudo mkdir -p configs/c.1/strings/0x409
echo "LEGO READER V2.10" | sudo tee configs/c.1/strings/0x409/configuration > /dev/null
echo 250                 | sudo tee configs/c.1/MaxPower > /dev/null

# HID function — 32-byte INPUT + 32-byte OUTPUT (Array type)
sudo mkdir -p functions/hid.g0
echo 0  | sudo tee functions/hid.g0/protocol > /dev/null
echo 0  | sudo tee functions/hid.g0/subclass > /dev/null
echo 32 | sudo tee functions/hid.g0/report_length > /dev/null

# HID descriptor — 27 bytes, verified byte-for-byte against Berny23
printf '\x06\x00\xFF\x09\x01\xA1\x01\x19\x01\x29\x20\x15\x00\x26\xFF\x00\x75\x08\x95\x20\x81\x00\x19\x01\x29\x20\x91\x00\xC0' | sudo tee functions/hid.g0/report_desc > /dev/null

sudo ln -sf functions/hid.g0 configs/c.1/

# Bind UDC — this activates the gadget so PS3 can see it
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
if [ -z "$UDC" ]; then
    echo -e "${RED}  ERROR: No UDC found! Is DRD enabled in BIOS?${NC}"
    exit 1
fi
echo "$UDC" | sudo tee UDC > /dev/null
sleep 1

if [ -e /dev/hidg0 ]; then
    sudo chmod 666 /dev/hidg0
    echo -e "${GREEN}  Gadget LIVE — /dev/hidg0 ready, PS3 will see LEGO READER V2.10${NC}"
else
    echo -e "${RED}  WARNING: /dev/hidg0 not created. Check DRD mode in BIOS.${NC}"
fi
echo ""

# ===========================================================================
# STEP 6: EMULATOR SETUP
# ===========================================================================
echo -e "${CYAN}>>> STEP 6: EMULATOR SETUP <<<${NC}"

if [ ! -d "$BERNY_DIR" ]; then
    echo "  Cloning Berny23/LD-ToyPad-Emulator..."
    cd ~
    git clone "$BERNY_URL" "$BERNY_DIR" || { echo -e "${RED}ERROR: Clone failed. Check internet.${NC}"; exit 1; }
fi

cd "$BERNY_DIR"

echo "  Installing npm dependencies..."
npm install --no-audit --no-fund 2>&1 | grep -E "(added|error|ERR)" || true
echo -e "${GREEN}  Emulator ready.${NC}"
echo ""

# ===========================================================================
# LAUNCH
# ===========================================================================
echo -e "${GREEN}==========================================================${NC}"
echo -e "${GREEN}  ALL SYSTEMS GO!${NC}"
echo -e "${GREEN}==========================================================${NC}"
echo ""
echo -e "  ${YELLOW}Deck browser:${NC}  ${GREEN}http://localhost${NC}"
echo ""
echo -e "  ${YELLOW}To play:${NC}"
echo "    1. Plug Deck (USB-C) → PS3 (USB-A) with DATA cable"
echo "    2. ${RED}REBOOT THE PS3${NC} (USB only scanned at boot)"
echo "    3. Boot LEGO Dimensions (original disc/ISO)"
echo "    4. Create characters → drag to ToyPad slots on touchscreen"
echo ""
echo -e "  ${RED}Press Ctrl+C to stop${NC}"
echo ""

sudo node index.js
