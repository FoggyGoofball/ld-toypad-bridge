# Steam Deck LEGO Dimensions ToyPad Emulator

Turns your Steam Deck into a physical LEGO Dimensions ToyPad via USB gadget mode.
Plug into PS3 (or PS4/Wii U) and the console sees a real ToyPad — no game mods needed.

## Quick Start

### 1. BIOS Setup (once)

Shut down → hold **Volume Up + Power** → **Setup Utility** → **Advanced** → **USB Configuration** → set **USB Dual-Role Device** to **DRD** → Save & Exit.

### 2. Run the script

Boot Desktop Mode, open **Konsole**, and run:

```bash
chmod +x deck_toypad.sh
sudo ./deck_toypad.sh
```

### 3. Play

1. Connect Steam Deck to PS3 with a **USB-C to USB-A data cable** (not charge-only)
2. Boot LEGO Dimensions (original unmodified disc/ISO)
3. Open `http://localhost` on the Deck's browser
4. Tap characters on the touchscreen — they appear in the game

## What It Does

| Step | Action |
|------|--------|
| Install Node.js | Via pacman (if missing) |
| USB Gadget | Creates `/sys/kernel/config/usb_gadget/g1` with VID `0x0E6F` PID `0x0241` |
| HID Descriptor | 80-byte input, 8-byte output — matches real ToyPad exactly |
| Clone Emulator | Berny23/LD-ToyPad-Emulator from GitHub |
| Start Server | Node.js server reading/writing `/dev/hidg0` |

## Requirements

- Steam Deck (LCD or OLED) running SteamOS
- USB-C to USB-A **data** cable (many cables are charge-only — test with a phone first)
- PS3/PS4/Wii U with LEGO Dimensions

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "No UDC found" | BIOS USB Dual-Role not set to DRD |
| Console doesn't detect pad | Cable is charge-only; try a different cable |
| `pacman` fails | Run `sudo pacman-key --init && sudo pacman-key --populate archlinux` first |
| `/dev/hidg0` permission denied | Run `sudo chmod 666 /dev/hidg0` |

## Credits

- [Berny23/LD-ToyPad-Emulator](https://github.com/Berny23/LD-ToyPad-Emulator) — ToyPad protocol reverse engineering and emulator
- USB HID descriptor extracted from LEGO Dimensions ToyPad
