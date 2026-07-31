#!/bin/bash
# =============================================================================
# run.sh — Minimal Steam Deck LEGO Dimensions ToyPad launcher
# =============================================================================
# One command to rule them all:
#   curl -sSL https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/main/deck/run.sh | sudo bash
# =============================================================================
set -e
GADGET_DIR=/sys/kernel/config/usb_gadget/g1
BERNY_DIR="$HOME/LD-ToyPad-Emulator"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

echo -e "${GREEN}=== Steam Deck LEGO Dimensions ToyPad ===${NC}"

# 1. System deps (skip if already installed)
echo -e "${YELLOW}[1/4] Checking system...${NC}"
sudo steamos-readonly disable 2>/dev/null || true
command -v node &>/dev/null || { echo "  Installing node/npm/git..."; sudo pacman -Sy --noconfirm nodejs npm git base-devel 2>/dev/null; }
echo -e "  ${GREEN}Node $(node --version)${NC}"

# 2. USB gadget
echo -e "${YELLOW}[2/4] USB gadget...${NC}"
sudo modprobe libcomposite 2>/dev/null || true
[ -d "$GADGET_DIR" ] && { [ -f "$GADGET_DIR/UDC" ] && echo "" | sudo tee "$GADGET_DIR/UDC" >/dev/null 2>&1 || true; sleep 0.5; sudo rmdir "$GADGET_DIR"/configs/c.1/strings/0x409 "$GADGET_DIR"/configs/c.1 "$GADGET_DIR"/functions/hid.g0 "$GADGET_DIR"/strings/0x409 "$GADGET_DIR" 2>/dev/null || true; }

sudo mkdir -p "$GADGET_DIR" && cd "$GADGET_DIR"
echo 0x0e6f | sudo tee idVendor >/dev/null
echo 0x0241 | sudo tee idProduct >/dev/null
echo 0x0100 | sudo tee bcdDevice >/dev/null
echo 0x0200 | sudo tee bcdUSB >/dev/null
sudo mkdir -p strings/0x409
echo "P.D.P.000000"|sudo tee strings/0x409/serialnumber >/dev/null
echo "PDP LIMITED. "|sudo tee strings/0x409/manufacturer >/dev/null
echo "LEGO READER V2.10"|sudo tee strings/0x409/product >/dev/null
sudo mkdir -p configs/c.1/strings/0x409
echo "LEGO READER V2.10"|sudo tee configs/c.1/strings/0x409/configuration >/dev/null
echo 250|sudo tee configs/c.1/MaxPower >/dev/null
sudo mkdir -p functions/hid.g0
echo 0|sudo tee functions/hid.g0/protocol >/dev/null
echo 0|sudo tee functions/hid.g0/subclass >/dev/null
echo 32|sudo tee functions/hid.g0/report_length >/dev/null
printf '\x06\x00\xFF\x09\x01\xA1\x01\x19\x01\x29\x20\x15\x00\x26\xFF\x00\x75\x08\x95\x20\x81\x00\x19\x01\x29\x20\x91\x00\xC0'|sudo tee functions/hid.g0/report_desc >/dev/null
sudo ln -sf functions/hid.g0 configs/c.1/
echo $(ls /sys/class/udc|head -1)|sudo tee UDC >/dev/null
sleep 1
[ -e /dev/hidg0 ] && { sudo chmod 666 /dev/hidg0; echo -e "  ${GREEN}/dev/hidg0 ready${NC}"; } || echo -e "  ${RED}No /dev/hidg0 — check BIOS DRD${NC}"

# 3. Berny23 emulator
echo -e "${YELLOW}[3/4] Berny23 emulator...${NC}"
[ ! -d "$BERNY_DIR" ] && git clone https://github.com/Berny23/LD-ToyPad-Emulator.git "$BERNY_DIR"
cd "$BERNY_DIR"
npm install --no-audit --no-fund 2>&1|grep -E "(added|error|ERR)"||true

# 4. Launch
echo -e "${GREEN}[4/4] Starting — open http://localhost on Deck browser${NC}"
echo -e "  ${YELLOW}Plug Deck→PS3, REBOOT PS3, start the game${NC}"
exec sudo node index.js
