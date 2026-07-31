#!/bin/bash
# =============================================================================
# update_deck.sh — Pull latest LD-ToyPad-Bridge and restart
# =============================================================================
# Run this whenever you want the latest version from GitHub.
#   cd ~/Desktop/ld-toypad-bridge/deck
#   ./update_deck.sh
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  LD-ToyPad-Bridge — Update & Restart${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""

# Check for root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Must run as root (needed for USB gadget).${NC}"
    echo "  sudo ./update_deck.sh"
    exit 1
fi

# Pull latest
echo -e "${YELLOW}Pulling latest from GitHub...${NC}"
cd "$REPO_DIR"
git pull origin main
echo -e "${GREEN}  Up to date.${NC}"

# Kill any running emulator
echo -e "${YELLOW}Stopping any running emulator...${NC}"
pkill -f "node index.js" 2>/dev/null && echo "  Stopped node server." || echo "  No server running."
sleep 1

# Run the setup & start script
echo ""
cd "$SCRIPT_DIR"
exec ./deck_toypad.sh
