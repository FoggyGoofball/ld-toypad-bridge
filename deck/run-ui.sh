#!/bin/bash
# =============================================================================
# run-ui.sh — LD-ToyPad Bridge UI version (custom overlay)
# =============================================================================
# curl -sSL https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/main/deck/run-ui.sh | sudo bash
# =============================================================================
set -e
GADGET_DIR=/sys/kernel/config/usb_gadget/g1
DECK_HOME=/home/deck
BERNY_DIR="$DECK_HOME/LD-ToyPad-Emulator-ui"
OVERLAY_BASE="https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/main/deck/overlay"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

echo -e "${GREEN}=== LD-ToyPad Bridge UI ===${NC}"

# 1. System deps
echo -e "${YELLOW}[1/4] System...${NC}"
sudo steamos-readonly disable 2>/dev/null || true
command -v node &>/dev/null || { echo "  Installing node/git..."; sudo pacman -Sy --noconfirm nodejs npm git base-devel 2>/dev/null; }
echo -e "  ${GREEN}Node $(node --version)${NC}"

# 2. USB gadget
echo -e "${YELLOW}[2/4] USB gadget...${NC}"
sudo modprobe libcomposite 2>/dev/null || true
if [ -d "$GADGET_DIR" ]; then
    [ -f "$GADGET_DIR/UDC" ] && echo "" | sudo tee "$GADGET_DIR/UDC" >/dev/null 2>&1 || true
    sleep 1
    sudo rm -f "$GADGET_DIR/configs/c.1/hid.g0" 2>/dev/null || true
    sudo rmdir "$GADGET_DIR/configs/c.1/strings/0x409" 2>/dev/null || true
    sudo rmdir "$GADGET_DIR/configs/c.1" 2>/dev/null || true
    sudo rmdir "$GADGET_DIR/functions/hid.g0" 2>/dev/null || true
    sudo rmdir "$GADGET_DIR/strings/0x409" 2>/dev/null || true
    sudo rmdir "$GADGET_DIR" 2>/dev/null || true
fi
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
UDC=$(ls /sys/class/udc 2>/dev/null|head -1)
[ -z "$UDC" ] && { echo -e "${RED}No UDC! Check BIOS DRD.${NC}"; exit 1; }
echo "$UDC"|sudo tee UDC >/dev/null
sleep 1
[ -e /dev/hidg0 ] && { sudo chmod 666 /dev/hidg0; echo -e "  ${GREEN}/dev/hidg0 ready${NC}"; } || { echo -e "${RED}/dev/hidg0 missing${NC}"; exit 1; }

# 3. Emulator with custom UI
echo -e "${YELLOW}[3/4] Emulator + UI overlay...${NC}"
if [ ! -d "$BERNY_DIR" ]; then
    git clone --depth 1 https://github.com/Berny23/LD-ToyPad-Emulator.git "$BERNY_DIR"
fi
cd "$BERNY_DIR"
echo "  Applying custom UI..."
curl -sSL "$OVERLAY_BASE/index.html" -o server/index.html
curl -sSL "$OVERLAY_BASE/main.css" -o server/stylesheets/main.css
curl -sSL "$OVERLAY_BASE/main.js" -o server/scripts/main.js
curl -sSL "$OVERLAY_BASE/sync-images.js" -o server/sync-images.js
curl -sSL "$OVERLAY_BASE/sync-api.js" -o server/sync-api.js
mkdir -p server/images
# Inject sync-api endpoint into root index.js (where app = express() is defined)
if ! grep -q "sync-api" index.js 2>/dev/null; then
  echo "" >> index.js
  echo "// Injected by LD-ToyPad Bridge overlay — on-demand image sync endpoint" >> index.js
  echo "try { require('./server/sync-api')(app); } catch(e) { console.error('sync-api load failed:', e.message); }" >> index.js
fi
echo "  npm install..."
npm install --no-audit --no-fund 2>&1|grep -E "(added|error|ERR)"||true

# 4. Launch
echo -e "${GREEN}=== Starting — http://localhost ===${NC}"
echo -e "  ${YELLOW}Plug Deck→PS3, REBOOT PS3, start game${NC}"
exec sudo node index.js
