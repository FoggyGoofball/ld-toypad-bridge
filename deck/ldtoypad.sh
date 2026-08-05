#!/bin/bash
# =============================================================================
# ldtoypad.sh — LD-ToyPad Bridge Unified Launcher
# =============================================================================
# Presents a menu to choose:
#   1. Start vanilla Berny23             (run.sh)
#   2. Start Berny23 with custom UI      (run-ui.sh)
#   3. Pair Steam Deck as DS3 controller (FFS daemon)
#   4. Start DS3 gamepad spoof           (Bluetooth DS3 + evdev daemon)
#   5. Custom UI + DS3 controller        (run-ui-sync-ds3.sh)
#
# Usage:
#   curl -sSL https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/v9.2.3/deck/ldtoypad.sh | sudo bash
# =============================================================================
set -e  # exit on error (except menu reads — those use /dev/tty)

DECK_HOME="/home/deck"
GITHUB_BASE="https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/v9.3.9/deck"
LOCAL_DIR="/tmp/ldtoypad"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
BOLD='\033[1m'

# ── Pre-download all scripts to a fixed temp directory ─────────
# This avoids the curl|bash SCRIPT_DIR cascade problem entirely.
# All scripts live in $LOCAL_DIR with known absolute paths.
# Cache with version stamp — re-download if version changed
VERSION_STAMP="$LOCAL_DIR/.version"
EXPECTED_VERSION="v9.3.9"
if [ ! -f "$VERSION_STAMP" ] || [ "$(cat "$VERSION_STAMP")" != "$EXPECTED_VERSION" ]; then
    echo "  Downloading scripts ($EXPECTED_VERSION)..."
    rm -rf "$LOCAL_DIR"
    mkdir -p "$LOCAL_DIR"
fi
fetch() {
    local name="$1"; local dest="$LOCAL_DIR/$name"
    if [ ! -f "$dest" ]; then
        curl -sSLf "$GITHUB_BASE/$name" -o "$dest" || { echo "  ERROR: Failed to download $name"; exit 1; }
        chmod +x "$dest"
    fi
}
echo "  Checking scripts..."
fetch run.sh
fetch run-ui.sh
fetch ds3-pair-daemon.c
fetch bt-connect-ds3.sh
fetch ds3-gamepad-daemon.py
fetch run-ui-sync-ds3.sh
echo "$EXPECTED_VERSION" > "$VERSION_STAMP"
echo ""

# Ensure root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Must run as root (sudo).${NC}"
    exit 1
fi

# Disable SteamOS read-only if needed
if steamos-readonly status 2>/dev/null | grep -q enabled; then
    echo -e "${YELLOW}Disabling read-only filesystem...${NC}"
    steamos-readonly disable
fi

# ── Ensure base system deps (idempotent, runs once) ────────────
ensure_base_deps() {
    # pacman keyring (first-boot issue on vanilla Deck)
    if ! pacman -Q pacman 2>/dev/null | grep -q pacman; then
        pacman-key --init 2>/dev/null || true
        pacman-key --populate archlinux 2>/dev/null || true
    fi
    # Python pip (needed for evdev in options 4/5)
    if ! command -v pip &>/dev/null && ! python3 -m pip --version &>/dev/null 2>&1; then
        echo "  Installing python-pip..."
        pacman -Sy --noconfirm python-pip 2>/dev/null || true
    fi
    # BlueZ utilities (needed for btmgmt/bluetoothctl in options 4/5)
    if ! command -v btmgmt &>/dev/null || ! command -v bluetoothctl &>/dev/null; then
        echo "  Installing bluez-utils..."
        pacman -Sy --noconfirm bluez-utils 2>/dev/null || true
    fi
}
ensure_base_deps

# ── Display menu ───────────────────────────────────────────────
show_menu() {
    clear
    echo ""
    echo -e "${GREEN}${BOLD}╔══════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}${BOLD}║   LEGO Dimensions ToyPad — Steam Deck Bridge ║${NC}"
    echo -e "${GREEN}${BOLD}╚══════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "  ${CYAN}${BOLD}1${NC})  ${BOLD}Start vanilla Berny23${NC}"
    echo -e "       └─ Upstream UI, single ToyPad gadget"
    echo ""
    echo -e "  ${CYAN}${BOLD}2${NC})  ${BOLD}Start Berny23 with Custom UI${NC}"
    echo -e "       └─ Our overlay, ToyPad gadget, touch-optimized"
    echo ""
    echo -e "  ${CYAN}${BOLD}3${NC})  ${BOLD}Pair Steam Deck as DS3 Controller${NC}"
    echo -e "       └─ One-time FFS USB pairing → saves BT MACs"
    echo ""
    echo -e "  ${CYAN}${BOLD}4${NC})  ${BOLD}Start DS3 Gamepad Spoof${NC}"
    echo -e "       └─ Bluetooth DS3 + evdev→HID daemon (no ToyPad)"
    echo ""
    echo -e "  ${CYAN}${BOLD}5${NC})  ${BOLD}Custom UI + DS3 Controller${NC}"
    echo -e "       └─ ToyPad gadget + wireless DS3 (full setup)"
    echo ""
    echo -e "  ${CYAN}${BOLD}0${NC})  ${BOLD}Exit${NC}"
    echo ""
    echo -ne "  ${GREEN}Choice [0-5]:${NC} "
}

# ── Option handlers ────────────────────────────────────────────

opt_vanilla() {
    echo -e "\n${GREEN}=== Starting Vanilla Berny23 ===${NC}"
    echo "  USB: ToyPad gadget (0x0E6F:0x0241, 32/32)"
    echo "  UI:  Upstream Berny23 (jQuery, drag-and-drop)"
    echo "  URL: http://localhost"
    echo ""
    exec bash "$LOCAL_DIR/run.sh"
}

opt_custom_ui() {
    echo -e "\n${GREEN}=== Starting Berny23 + Custom UI ===${NC}"
    echo "  USB: ToyPad gadget (0x0E6F:0x0241, 32/32)"
    echo "  UI:  LD-ToyPad Bridge overlay (vanilla JS, touch-optimized)"
    echo "  URL: http://localhost"
    echo ""
    exec bash "$LOCAL_DIR/run-ui.sh"
}

opt_pair_ds3() {
    echo -e "\n${GREEN}=== Pair Steam Deck as DS3 Controller ===${NC}"
    echo "  This is a ONE-TIME setup step (30 seconds)."
    echo "  You only need to run this once — pairing survives reboots."
    echo ""

    PAIRING_FILE="$DECK_HOME/.config/ld-toypad/ds3-pairing.json"
    if [ -f "$PAIRING_FILE" ]; then
        echo -e "  ${YELLOW}Pairing data already exists:${NC}"
        echo "    Deck BT: $(grep -oP '"deck_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")"
        echo "    PS3 BT:  $(grep -oP '"ps3_bt_mac":\s*"\K[^"]+' "$PAIRING_FILE")"
        echo ""
        echo -ne "  Re-pair? This will overwrite existing pairing. [y/N]: "
        read -r confirm < /dev/tty
        if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
            echo "  Skipped."
            return
        fi
    fi

    # Ensure gcc
    if ! command -v gcc &>/dev/null; then
        echo "  Installing gcc..."
        pacman -Sy --noconfirm gcc 2>/dev/null
    fi

    # Compile daemon (fetch source if piped from curl)
    gcc -Wall -O2 -o /tmp/ds3-pair-daemon "$LOCAL_DIR/ds3-pair-daemon.c"

    echo ""
    echo -e "  ${YELLOW}╔══════════════════════════════════════════════╗${NC}"
    echo -e "  ${YELLOW}║  PLUG USB-C CABLE INTO PS3 NOW               ║${NC}"
    echo -e "  ${YELLOW}║  PS3 must be powered on                      ║${NC}"
    echo -e "  ${YELLOW}╚══════════════════════════════════════════════╝${NC}"
    echo ""

    /tmp/ds3-pair-daemon
    local ret=$?

    if [ $ret -eq 0 ] && [ -f "$PAIRING_FILE" ]; then
        echo -e "\n${GREEN}✓ Pairing successful!${NC}"
        echo "  Data saved to: $PAIRING_FILE"
    else
        echo -e "\n${RED}✗ Pairing failed (exit code $ret).${NC}"
    fi

    echo ""
    echo -ne "  Press Enter to return to menu..."
    read -r < /dev/tty
}

opt_gamepad_spoof() {
    echo -e "\n${GREEN}=== Start DS3 Gamepad Spoof ===${NC}"
    echo "  Bluetooth: DS3 controller (your Deck controls → PS3)"
    echo "  USB:       None (ToyPad NOT active)"
    echo ""

    PAIRING_FILE="$DECK_HOME/.config/ld-toypad/ds3-pairing.json"
    if [ ! -f "$PAIRING_FILE" ]; then
        echo -e "  ${RED}No pairing data found.${NC}"
        echo "  Run option 3 first to pair with PS3."
        echo -ne "  Press Enter to return to menu..."
        read -r < /dev/tty
        return
    fi

    # 1. Connect Bluetooth
    echo "  Connecting Bluetooth..."
    bash "$LOCAL_DIR/bt-connect-ds3.sh"
    local bt_ret=$?
    if [ $bt_ret -ne 0 ]; then
        echo -e "  ${RED}Bluetooth connection failed.${NC}"
        echo -ne "  Press Enter to return to menu..."
        read -r < /dev/tty
        return
    fi

    # 2. Install evdev
    if ! python3 -c "import evdev" 2>/dev/null; then
        echo "  Installing evdev..."
        pip install evdev 2>/dev/null || pacman -S --noconfirm python-evdev 2>/dev/null
    fi

    # 3. Start daemon
    echo ""
    echo -e "  ${GREEN}Starting DS3 gamepad daemon...${NC}"
    echo "  Hotkey: L4 + R4 to toggle between PS3 mode and Desktop mode."
    echo "  Press Ctrl+C to stop."
    echo ""
    exec python3 "$LOCAL_DIR/ds3-gamepad-daemon.py"
}

opt_custom_ds3() {
    echo -e "\n${GREEN}=== Custom UI + DS3 Controller ===${NC}"
    echo "  USB:       ToyPad gadget (0x0E6F:0x0241, 32/32)"
    echo "  Bluetooth: DS3 controller (Deck controls → PS3)"
    echo "  UI:        LD-ToyPad Bridge overlay"
    echo "  URL:       http://localhost"
    echo ""

    PAIRING_FILE="$DECK_HOME/.config/ld-toypad/ds3-pairing.json"
    if [ ! -f "$PAIRING_FILE" ]; then
        echo -e "  ${RED}No pairing data found.${NC}"
        echo "  Run option 3 first to pair with PS3."
        echo -ne "  Press Enter to return to menu..."
        read -r < /dev/tty
        return
    fi

    exec bash "$LOCAL_DIR/run-ui-sync-ds3.sh"
}

# ── Main loop ──────────────────────────────────────────────────
while true; do
    show_menu
    read -r choice < /dev/tty

    case "$choice" in
        1) opt_vanilla ;;
        2) opt_custom_ui ;;
        3) opt_pair_ds3 ;;
        4) opt_gamepad_spoof ;;
        5) opt_custom_ds3 ;;
        0) echo -e "\n${GREEN}Goodbye.${NC}"; exit 0 ;;
        *) echo -e "${RED}Invalid choice. Press Enter...${NC}"; read -r < /dev/tty ;;
    esac
done
