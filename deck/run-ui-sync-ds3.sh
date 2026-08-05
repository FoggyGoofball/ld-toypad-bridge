#!/bin/bash
# =============================================================================
# run-ui-sync-ds3.sh — LD-ToyPad Bridge UI + DS3 Bluetooth pairing
# =============================================================================
# Clone of run-ui.sh with an additional --pair-controller mode that uses
# ds3-pair-daemon.c (FunctionFS) for one-time DS3 Bluetooth pairing with PS3.
#
# MODES:
#   Normal (ToyPad only — identical to run-ui.sh):
#     curl -sSL .../run-ui-sync-ds3.sh | sudo bash
#
#   Pair controller (one-time setup, then ToyPad):
#     curl -sSL .../run-ui-sync-ds3.sh | sudo bash -s -- --pair-controller
#
#   Pair controller only (no ToyPad, for testing):
#     curl -sSL .../run-ui-sync-ds3.sh | sudo bash -s -- --pair-controller-only
#
# WORKFLOW:
#   1. Run once with --pair-controller → FFS USB pairing → saves pairing.json
#   2. Script auto-connects Bluetooth to PS3 (btmgmt MAC spoof + bluetoothctl)
#   3. Builds ToyPad gadget, applies overlay, starts emulator
#   4. Every session: just run without flags → ToyPad + DS3 auto-connect
# =============================================================================
set -e

GADGET_DIR=/sys/kernel/config/usb_gadget/g1
DECK_HOME=/home/deck
BERNY_DIR="$DECK_HOME/LD-ToyPad-Emulator-ui"
OVERLAY_BASE="https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/main/deck/overlay"
SCRIPTS_BASE="https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/main/deck"

# When piped from curl, sibling scripts don't exist locally
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dl() {
    local name="$1"
    if [ -f "$SCRIPT_DIR/$name" ]; then echo "$SCRIPT_DIR/$name"; return; fi
    local p="/tmp/ldtoypad-$name"
    if [ ! -f "$p" ]; then curl -sSL "$SCRIPTS_BASE/$name" -o "$p"; chmod +x "$p"; fi
    echo "$p"
}
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

# ── Parse mode ──────────────────────────────────────────────────
MODE="toypad"
if [ "${1:-}" = "--pair-controller" ]; then MODE="pair-and-toypad"; fi
if [ "${1:-}" = "--pair-controller-only" ]; then MODE="pair-only"; fi

echo -e "${GREEN}=== LD-ToyPad Bridge UI${NC}"
[ "$MODE" != "toypad" ] && echo -e "${GREEN}   + DS3 Controller Pairing${NC}"
echo ""

# ── 1. System deps ─────────────────────────────────────────────
echo -e "${YELLOW}[1/5] System dependencies...${NC}"
sudo steamos-readonly disable 2>/dev/null || true

if ! command -v node &>/dev/null; then
    echo "  Installing node/git/build-tools..."
    sudo pacman -Sy --noconfirm nodejs npm git base-devel 2>/dev/null
fi
echo -e "  ${GREEN}Node $(node --version)${NC}"

if [ "$MODE" != "toypad" ]; then
    if ! command -v gcc &>/dev/null; then
        echo "  Installing gcc (for FFS daemon)..."
        sudo pacman -S --noconfirm gcc 2>/dev/null
    fi
    if ! command -v hciconfig &>/dev/null; then
        echo "  Installing bluez-utils (for Bluetooth)..."
        sudo pacman -S --noconfirm bluez bluez-utils 2>/dev/null
    fi
fi

# ── 2. DS3 Pairing (if --pair-controller) ──────────────────────
if [ "$MODE" != "toypad" ]; then
    echo -e "${YELLOW}[2/5] DS3 Bluetooth Pairing (one-time setup)...${NC}"

    PAIRING_FILE="$DECK_HOME/.config/ld-toypad/ds3-pairing.json"

    if [ -f "$PAIRING_FILE" ]; then
        echo -e "  ${GREEN}Pairing data already exists.${NC}"
        echo "  Deck BT: $(grep -oP '"deck_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")"
        echo "  PS3 BT:  $(grep -oP '"ps3_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")"
        echo "  To re-pair: rm $PAIRING_FILE and re-run."
    else
        echo "  Downloading ds3-pair-daemon.c..."
        curl -sSL "$SCRIPTS_BASE/ds3-pair-daemon.c" -o /tmp/ds3-pair-daemon.c

        echo "  Compiling..."
        gcc -Wall -O2 -o /tmp/ds3-pair-daemon /tmp/ds3-pair-daemon.c
        echo -e "  ${GREEN}Compiled.${NC}"

        echo ""
        echo -e "  ${YELLOW}=============================================${NC}"
        echo -e "  ${YELLOW}  PLUG USB-C CABLE INTO PS3 NOW${NC}"
        echo -e "  ${YELLOW}=============================================${NC}"
        echo ""

        sudo /tmp/ds3-pair-daemon
        PAIR_EXIT=$?

        if [ $PAIR_EXIT -eq 0 ] && [ -f "$PAIRING_FILE" ]; then
            echo -e "  ${GREEN}Pairing successful!${NC}"
        else
            echo -e "  ${RED}Pairing failed (exit code $PAIR_EXIT).${NC}"
            if [ "$MODE" = "pair-only" ]; then exit 1; fi
            echo "  Continuing with ToyPad setup anyway..."
        fi
    fi

    if [ "$MODE" = "pair-only" ]; then
        echo ""
        echo -e "${GREEN}=== Pairing complete. ===${NC}"
        echo ""
        echo "  Next steps:"
        echo "    Run ldtoypad.sh and select option 4 or 5 to use the DS3"
        exit 0
    fi
fi

# ── 2b. Auto-connect Bluetooth DS3 (if pairing data exists) ─────
PAIRING_FILE="$DECK_HOME/.config/ld-toypad/ds3-pairing.json"
BT_STEP=$([ "$MODE" != "toypad" ] && echo "3" || echo "2")
if [ -f "$PAIRING_FILE" ]; then
    echo -e "${YELLOW}[$BT_STEP/5] Bluetooth DS3 connection...${NC}"
    # Auto-connect to PS3 as wireless DS3 (expert confirmed: Deck initiates L2CAP)
    if command -v btmgmt &>/dev/null && command -v bluetoothctl &>/dev/null; then
        bash "$(dl bt-connect-ds3.sh)" 2>/dev/null && \
            echo -e "  ${GREEN}Wireless DS3 connected${NC}" || \
            echo -e "  ${YELLOW}BT connection skipped (PS3 may be off)${NC}"
    else
        echo -e "  ${YELLOW}btmgmt/bluetoothctl not found. Install: pacman -S bluez-utils${NC}"
    fi
else
    echo -e "  ${YELLOW}No DS3 pairing found. Run with --pair-controller first.${NC}"
fi

# ── 3. USB ToyPad Gadget ────────────────────────────────────────
TOYPAD_STEP=$([ "$MODE" != "toypad" ] && echo "4" || echo "3")
echo -e "${YELLOW}[$TOYPAD_STEP/5] USB ToyPad gadget...${NC}"

# Tear down any leftover FFS gadget from pairing daemon
if [ -d /sys/kernel/config/usb_gadget/ds3-pair ]; then
    [ -f /sys/kernel/config/usb_gadget/ds3-pair/UDC ] && echo "" | sudo tee /sys/kernel/config/usb_gadget/ds3-pair/UDC >/dev/null 2>&1 || true
    sudo umount /dev/ffs-ds3 2>/dev/null || true
    sudo rm -rf /sys/kernel/config/usb_gadget/ds3-pair 2>/dev/null || true
    sudo rm -rf /dev/ffs-ds3 2>/dev/null || true
fi

sudo modprobe libcomposite 2>/dev/null || true

# Tear down any existing gadget (could be leftover from pairing daemon)
if [ -d "$GADGET_DIR" ]; then
    [ -f "$GADGET_DIR/UDC" ] && echo "" | sudo tee "$GADGET_DIR/UDC" >/dev/null 2>&1 || true
    sleep 1
    sudo rm -f "$GADGET_DIR/configs/c.1/hid.g0" 2>/dev/null || true
    sudo rm -f "$GADGET_DIR/configs/c.1/ffs.usb0" 2>/dev/null || true
    sudo rmdir "$GADGET_DIR/configs/c.1/strings/0x409" 2>/dev/null || true
    sudo rmdir "$GADGET_DIR/configs/c.1" 2>/dev/null || true
    sudo rmdir "$GADGET_DIR/functions/hid.g0" 2>/dev/null || true
    sudo rmdir "$GADGET_DIR/functions/ffs.usb0" 2>/dev/null || true
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

# ── 4. Emulator with custom UI ──────────────────────────────────
EMU_STEP=$([ "$MODE" != "toypad" ] && echo "5" || echo "4")
echo -e "${YELLOW}[$EMU_STEP/5] Emulator + UI overlay...${NC}"
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
if ! grep -q "sync-api" index.js 2>/dev/null; then
  echo "" >> index.js
  echo "// Injected by LD-ToyPad Bridge overlay — on-demand image sync endpoint" >> index.js
  echo "try { require('./server/sync-api')(app); } catch(e) { console.error('sync-api load failed:', e.message); }" >> index.js
fi
echo "  npm install..."
npm install --no-audit --no-fund 2>&1|grep -E "(added|error|ERR)"||true

# ── 5. Launch ──────────────────────────────────────────────────
STEP=$([ "$MODE" != "toypad" ] && echo "5" || echo "4")
echo ""
echo -e "${GREEN}=== Starting — http://localhost ===${NC}"
if [ -f "$DECK_HOME/.config/ld-toypad/ds3-pairing.json" ]; then
    echo -e "  ${GREEN}DS3: Wireless controller active${NC}"
    echo -e "  ${YELLOW}Hotkey: L4+R4 to toggle between PS3 mode and Desktop mode${NC}"
fi
echo -e "  ${YELLOW}Plug Deck→PS3, REBOOT PS3, start game${NC}"
exec sudo node index.js
