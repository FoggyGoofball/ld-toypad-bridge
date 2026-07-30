# Steam Deck LEGO Dimensions ToyPad Emulator

Turns your Steam Deck into a physical LEGO Dimensions ToyPad via USB gadget mode.
Plug into PS3 (or PS4/Wii U) and the console sees a real ToyPad — **no game mods needed.**

---

## BEFORE YOU START — Read This First

### What you need

| Item | Why |
|------|-----|
| Steam Deck (LCD or OLED) | This is the ToyPad |
| USB-C to USB-A cable | Connects Deck to console. **Must be a DATA cable** — many are charge-only! |
| LEGO Dimensions (original, unmodified) | On PS3, PS4, or Wii U |
| Wi-Fi on the Deck | To download the emulator |

### How to test your USB-C cable

Plug the cable between your Deck and a PC or phone. If the PC/phone shows a new device connected, it's a data cable. If nothing happens, it's charge-only — get a different cable.

### If you've never used Desktop Mode before

1. Press the **STEAM** button
2. Scroll to **Power** → **Switch to Desktop**
3. The Deck will switch to a Windows-like desktop
4. Click the bottom-left **Application Launcher** (Steam logo) → type **Konsole** → click it
5. You now have a terminal. This is where you paste commands.

### You need a sudo password

If you've never set one, in Konsole type `passwd`, press Enter, type a password twice. **Remember it** — you'll need it for every `sudo` command.

---

## Step-by-Step Setup

### Step 1: BIOS (one-time, 30 seconds)

1. Shut down your Steam Deck completely (**Steam button → Power → Shutdown**)
2. Hold **Volume Up (+)** and press **Power** — keep holding Vol Up until the menu appears
3. Select **Setup Utility**
4. Navigate to **Advanced → USB Configuration**
5. Change **USB Dual-Role Device** to **DRD**
6. Press **Select** (≡ button, top-left) to Save and Exit
7. The Deck reboots normally

**How to know it worked:** After booting, run `ls /sys/class/udc` in Konsole. If you see something like `38100000.usb`, it's correct. If empty, the BIOS setting didn't take — repeat Step 1.

### Step 2: Get the files onto your Deck

**Option A: Clone from GitHub (easiest, needs Wi-Fi)**
```bash
cd ~/Desktop
git clone https://github.com/FoggyGoofball/ld-toypad-bridge.git
cd ld-toypad-bridge/deck
```

**Option B: Download ZIP (if git isn't installed)**
Open Firefox on the Deck, go to `https://github.com/FoggyGoofball/ld-toypad-bridge`, click the green **Code** button → **Download ZIP**. Extract to Desktop, open the `deck` folder in Konsole.

### Step 3: Run the setup script

In Konsole (in the `deck` folder):
```bash
chmod +x deck_toypad.sh
sudo ./deck_toypad.sh
```

**What you'll see:**
- `[1/5] Checking Node.js...` — installs Node if missing (may take 2-3 minutes on first run)
- `[2/5] Setting up USB gadget...` — creates the virtual ToyPad device
- `[3/5] Setting up emulator...` — downloads Berny23's emulator
- `[4/5] Starting ToyPad emulator...` — server goes live

### Step 4: Connect and play

1. Plug USB-C end into your **Steam Deck**, USB-A end into the **PS3**
2. On the PS3, boot LEGO Dimensions (original, unpatched)
3. On the Deck, open Firefox → go to **`http://localhost`**
4. You'll see the ToyPad grid. Tap characters — they appear in the game!

**How to know it worked:** The PS3 should skip the "Connect ToyPad" screen and go straight to the game. If you see "Connect the ToyPad," the cable might be charge-only or the gadget didn't bind.

---

## Troubleshooting

| Problem | Check this |
|---------|-----------|
| "No UDC found" in script | BIOS USB Dual-Role is not set to DRD. Repeat Step 1. |
| PS3 says "Connect ToyPad" | Cable is charge-only. Test it: plug Deck into PC — if PC doesn't see a new device, the cable is bad. Also try: `ls /sys/class/udc` — must not be empty. |
| `sudo: command not found` | You need to set a password first. Run `passwd` in Konsole. |
| `pacman: command not found` | You're not on SteamOS. This script is for Steam Deck only. |
| `pacman` key errors | Run these first: `sudo pacman-key --init` then `sudo pacman-key --populate archlinux`, then retry. |
| `steamos-readonly` errors | Run `sudo steamos-readonly disable` before the script. |
| `/dev/hidg0: Permission denied` | Run `sudo chmod 666 /dev/hidg0` |
| Web UI shows nothing | Make sure you're on `http://localhost` (not localhost:3000). If that fails, check `http://localhost:3000`. |
| Emulator starts but PS3 doesn't react | Reboot the PS3 with the Deck still plugged in. The PS3 only enumerates USB at boot. |
| Deck battery drains fast | Normal — USB gadget mode uses power. Keep the Deck plugged into its charger while playing. |

---

## What the Script Actually Does

| Step | Action | Why |
|------|--------|-----|
| Install Node.js | `pacman -S nodejs npm` | Needed to run the emulator |
| Load kernel module | `modprobe libcomposite` | Enables USB gadget support |
| Create gadget | Sets VID `0x0E6F` PID `0x0241` | PS3 looks for this exact hardware |
| HID descriptor | 80-byte input, 8-byte output | Matches real ToyPad exactly |
| Clone emulator | `Berny23/LD-ToyPad-Emulator` | Handles NFC crypto, tag simulation |
| Start server | `node index.js` | Reads/writes `/dev/hidg0` |

---

## Safety / Misc

- **This does NOT modify your PS3.** The PS3 sees a real USB ToyPad. No EBOOT patches, no CFW needed.
- **This does NOT modify your Steam Deck permanently.** Only the BIOS DRD setting persists (you can change it back).
- **To stop:** Press Ctrl+C in Konsole. The USB gadget disappears when the Deck powers off.
- **To re-run:** Just do Step 3 again (`sudo ./deck_toypad.sh`). The script cleans up any previous gadget.

## Credits

- [Berny23/LD-ToyPad-Emulator](https://github.com/Berny23/LD-ToyPad-Emulator) — ToyPad protocol reverse engineering and Node.js emulator
- USB HID descriptor from real LEGO Dimensions ToyPad hardware
