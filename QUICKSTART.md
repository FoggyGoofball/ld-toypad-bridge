# LD-ToyPad Bridge — Quickstart Guide

Turn your Steam Deck into a LEGO Dimensions ToyPad + wireless PS3 controller. **No game mods, no CFW, no EBOOT patches.**

---

## Before You Start

### What You Need

| Item | Why |
|------|-----|
| Steam Deck (LCD or OLED) | This becomes the ToyPad + controller |
| USB-C to USB-A **data** cable | Charge-only cables won't work — test by connecting Deck to a PC |
| LEGO Dimensions (original disc) | Unmodified PS3 game |
| Wi-Fi on the Deck | Downloads dependencies automatically |
| sudo password | If you haven't set one: `passwd` in Konsole |

### ⚠️ Step 0: BIOS DRD Mode (Required)

USB gadget mode must be enabled in BIOS. This is a **one-time setting**.

1. **Shut down** the Steam Deck completely
2. Hold **Volume Up** + press **Power** — release Power but keep holding Vol Up
3. In the Setup Utility, go to **Advanced** → **USB Configuration**
4. Change **USB Dual-Role Device** from `XHCI` to **`DRD`**
5. Press **Select** (☰ button) → **Save and Exit**
6. Deck reboots normally into SteamOS

> **Note:** If you ever reset your BIOS to defaults, you'll need to re-enable DRD.

---

## Option A: Full Bridge (ToyPad + Optional Controller)

One command. Zero pre-installed dependencies.

```bash
curl -sSL https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/v9.0/deck/ldtoypad.sh | sudo bash
```

You'll see a menu:

```
╔══════════════════════════════════════════════╗
║   LEGO Dimensions ToyPad — Steam Deck Bridge ║
╚══════════════════════════════════════════════╝

  1)  Start vanilla Berny23         ← original UI, ToyPad only
  2)  Start Berny23 with Custom UI  ← our overlay, ToyPad only
  3)  Pair Steam Deck as DS3        ← one-time Bluetooth pairing
  4)  Start DS3 Gamepad Spoof       ← controller only (any PS3 game)
  5)  Custom UI + DS3 Controller    ← full hybrid setup

  0)  Exit
```

### First Time Setup

1. Pick **option 3** — Pair DS3 Controller
2. When prompted, plug the USB-C cable from Deck to PS3 (PS3 must be on)
3. Wait ~30 seconds for "Pairing complete" message
4. Pick **option 5** — Custom UI + DS3 Controller

### Every Session After

Just pick **option 5**. Everything auto-connects.

### What Happens

- Steam Deck shows the ToyPad web UI at `http://localhost` (touchscreen)
- Deck connects to PS3 as a wireless DualShock 3 controller
- **L4 + R4** toggles between PS3 controller mode and desktop/touchscreen mode
- **L5 + R5** = PS button

---

## Option B: Controller Only (Any PS3 Game)

If you just want to use your Deck as a PS3 controller for *any* game:

```bash
curl -sSL https://raw.githubusercontent.com/FoggyGoofball/ld-toypad-bridge/v9.0/deck/deck-controller.sh | sudo bash
```

This does the same as options 3+4 above — one-time USB pairing, then wireless forever. No ToyPad, no LEGO Dimensions, no web UI.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "No UDC found" | BIOS DRD mode not set — redo Step 0 |
| "Connect ToyPad" on PS3 | Bad cable (charge-only) or gadget didn't bind. Try a different USB-C cable. |
| `/dev/hidg0: Permission denied` | Run with `sudo` |
| Bluetooth won't connect | PS3 must be powered on and on XMB. Deck initiates the connection. |
| MAC spoof fails | `btmgmt public-addr` is tried first (modern), then `hciconfig` (fallback). Both work on all Steam Deck chipsets. |
| Controller buttons don't work in-game | Press L4+R4 to toggle into PS3 controller mode |
| Page shows old UI after update | Hard refresh: `Ctrl+Shift+R` in browser |
| pacman keyring errors on first boot | Handled automatically by the launcher |

---

## How It Works

```
┌─────────────────────┐         Bluetooth          ┌──────────┐
│     Steam Deck      │ ← ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ →  │   PS3    │
│  Bluetooth radio ───┤   (DualShock 3 profile)    │ Player 1 │
│                     │         USB-C cable        │          │
│  USB Gadget ────────┤ ←───────────────────────→  │  LEGO    │
│  (ToyPad)           │   (VID 0x0E6F, PID 0x0241) │ Dimensions│
│                     │                            │ UNMODDED │
│  web UI ────────────┤  http://localhost          │          │
│  (touchscreen)      │                            │          │
└─────────────────────┘                            └──────────┘
```

- **USB:** The Deck impersonates the real LEGO Dimensions ToyPad at the hardware level (VID `0x0E6F`, PID `0x0241`, exact 32-byte HID descriptor). The PS3 sees a genuine ToyPad.
- **Bluetooth:** The Deck pairs as a DualShock 3 via a one-time FFS USB handshake, then connects wirelessly forever.
- **Web UI:** Berny23's LD-ToyPad-Emulator runs locally, serving a touch-optimized interface for placing virtual LEGO tags on the ToyPad zones.

---

## Manual Setup (Without the Launcher)

If you prefer to run individual scripts:

```bash
# Clone the repo
git clone https://github.com/FoggyGoofball/ld-toypad-bridge.git
cd ld-toypad-bridge/deck

# ToyPad only (vanilla upstream UI)
sudo ./run.sh

# ToyPad only (custom overlay UI)
sudo ./run-ui.sh

# One-time DS3 Bluetooth pairing
sudo ./ds3-pair-daemon    # (compile first: gcc -O2 -o ds3-pair-daemon ds3-pair-daemon.c)

# Bluetooth reconnection (after pairing)
sudo ./bt-connect-ds3.sh

# Gamepad daemon (evdev → DS3 over Bluetooth)
sudo python3 ds3-gamepad-daemon.py
```

---

## Repository

- **Code:** https://github.com/FoggyGoofball/ld-toypad-bridge
- **Upstream emulator:** https://github.com/Berny23/LD-ToyPad-Emulator
- **FFS pairing based on:** https://github.com/ihasTaco/RosettaPad
- **Full changelog:** [`docs/CHANGELOG-2026-08-05-overlay-ds3-pairing.md`](docs/CHANGELOG-2026-08-05-overlay-ds3-pairing.md)
