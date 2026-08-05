# Composite USB Gadget: Round 2 — Gadget Cycling & Dual-Role Questions

**Date:** 2026-08-04  
**Project:** LEGO Dimensions ToyPad Emulation for PS3  
**Context:** The composite gadget approach (simultaneous ToyPad + DS3 on one cable) was confirmed impossible by the expert due to device-level VID/PID clashing and configfs hidg's inability to handle ep0 DS3 authentication. We're now exploring alternative architectures.

---

## Expert's Confirmed Fatal Blockers (Recap)

| # | Blocker | Root Cause |
|---|---------|-----------|
| 1 | VID/PID clashing | Composite devices share a single Device Descriptor. Can't be both `0x0E6F:0x0241` (ToyPad) and `0x054C:0x0268` (DS3) simultaneously. No true hub emulation in configfs. |
| 2 | DS3 ep0 authentication | `usb_f_hid` (hidg) can't intercept Feature Reports on Endpoint 0. PS3 sends `GET_REPORT` (0xF2, 0xF5) and `SET_REPORT` (0xEF). Without correct responses, controller never gets a player slot — just flashing LEDs forever. |
| 3 | FunctionFS required | Proper DS3 emulation needs FunctionFS (FFS) — a custom userspace USB driver. Projects like RosettaPad and droidshock3 use this. SteamOS doesn't ship the needed kernel modules. |

---

## New Proposal: Gadget Cycling + Bluetooth Dual-Role

Instead of simultaneous composite, cycle through gadgets:

1. **Phase 1 (optional):** Steam Deck pairs to PS3 as a wireless controller via Bluetooth
2. **Phase 2:** USB-C port presents as ToyPad only (`0x0E6F:0x0241`, 32/32 HID) — our proven working setup
3. **Result:** Steam Deck is both the wireless controller (Bluetooth) AND the ToyPad (USB) simultaneously. No gadget-level VID/PID conflict.

```
┌─────────────────────┐         Bluetooth          ┌──────────┐
│     Steam Deck      │ ← ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ →  │   PS3    │
│                     │   (DS3/DS4 HID profile)    │          │
│  Bluetooth radio ───┤                            │ Player 1 │
│                     │         USB-C cable        │          │
│  USB Gadget ────────┤ ←───────────────────────→  │  LEGO    │
│  (ToyPad only)      │   (0x0E6F:0x0241, 32/32)  │ Dimensions│
└─────────────────────┘                            └──────────┘
```

---

## Questions

### 🔴 Question 1: Steam Deck Bluetooth as DS3/DS4 Controller

The Steam Deck runs Arch Linux with a Bluetooth 5.0 radio. The PS3 accepts DualShock 3 (Bluetooth HID) and DualShock 4 (Bluetooth, with limited feature support) as wireless controllers.

**Sub-question A — DS3 over Bluetooth:**
The DS3 uses Bluetooth HID profile with a specific authentication handshake. There are Linux projects that implement this:
- `sixad` / `QtSixA` — userspace daemon that pairs as a DS3 over Bluetooth
- `hid-sony` kernel module (mainline since Linux 4.12) — handles DS3/DS4 Bluetooth connections

Can the Steam Deck's Bluetooth radio be configured to present as a DualShock 3 to the PS3? Specifically:
- Does the Deck's Bluetooth firmware support the required HID profile?
- Has anyone successfully paired a Steam Deck as a DS3 to a PS3?
- Does the Deck's `hid-sony` module need any special configuration?

**Sub-question B — DS4 over Bluetooth (simpler path):**
The PS3 natively accepts DualShock 4 controllers over Bluetooth (since PS3 firmware 4.60). The DS4 uses standard Bluetooth HID without the DS3's proprietary authentication challenge. The Steam Deck's built-in controller (evdev) could be translated to DS4 HID reports and sent over Bluetooth.

Is Bluetooth DS4 emulation from the Deck more feasible than DS3? Specifically:
- Are there existing Linux tools (like `ds4drv`) that can run on SteamOS and bridge evdev → Bluetooth DS4 HID?
- Does the PS3 accept a DS4 connected via Bluetooth from a non-Sony device?
- Are there any known issues with DS4 on PS3 (missing PS button, no rumble, no SIXAXIS) that would make this impractical for LEGO Dimensions?

**Sub-question C — The Bluetooth coexistence problem:**
If the Deck is simultaneously acting as a USB gadget (ToyPad) AND a Bluetooth controller:
- Do the Deck's USB-C port (in DRD/gadget mode) and Bluetooth radio share any hardware resources (antenna, bus) that would cause interference?
- Is there a known issue with USB gadget mode and Bluetooth coexisting on the Steam Deck's specific hardware (Valve Jupiter/Aerith SoC)?
- Does the USB cable itself cause enough EMI to degrade Bluetooth signal quality at close range?

---

### 🔴 Question 2: Gadget Cycling — "Unplug and Rebuild"

The cycling approach:

```bash
# Phase 1: Present as DS3 (hypothetical — if we solve ep0 auth)
echo "" > /sys/kernel/config/usb_gadget/g1/UDC    # unbind
rm -rf /sys/kernel/config/usb_gadget/g1            # destroy
# ... rebuild as ToyPad ...
echo "$UDC" > /sys/kernel/config/usb_gadget/g1/UDC  # rebind
```

**Sub-question A — PS3's reaction to device disappearance:**
When the USB gadget is unbound from the UDC, the PS3 sees a physical USB disconnect. When rebuilt with a different VID/PID and rebound, the PS3 sees a new device plugged in.

- Does the PS3 re-enumerate USB devices at runtime, or only at boot? (Our HANDOFF doc notes: "USB hotplug: kernel filter blocks non-matching devices before they reach the game")
- If we "unplug" a DS3-emulating gadget and "plug in" a ToyPad gadget, does the game (`cellUsbdRegisterLdd`) re-scan for the ToyPad? The HANDOFF doc says: "XMB rescan: game doesn't re-register after initial boot"
- Would we need to restart the game between gadget switches?

**Sub-question B — Controller port assignment persistence:**
If the PS3 assigns controller port 1 to the USB gadget (while it's spoofing a DS3), then the gadget disappears:
- Does the PS3 release port 1 and allow a Bluetooth controller to claim it?
- Or does it hold port 1 in a "disconnected" state expecting the same device to return?

**Sub-question C — Our specific use case reversal:**
The original cycling idea was: spoof controller → then spoof ToyPad. But what about the reverse?
1. Boot game with ToyPad gadget (our proven working setup)
2. The game registers the ToyPad and starts
3. AFTER the game is running, tear down ToyPad gadget, rebuild as controller
4. Does the game continue running without the ToyPad present? Or does it freeze/crash when the USB device it registered disappears?

---

### 🟡 Question 3: Physical USB Hub Between Deck and PS3

If the Deck can't do both simultaneously via software, could a physical USB hub split the Deck's single USB-C port into two separate USB devices as far as the PS3 is concerned?

```
Steam Deck USB-C → [USB-C Hub with 2 downstream ports]
                        ├── Port 1: ToyPad gadget (configfs)
                        └── Port 2: DS3 gadget (configfs, if we solve ep0)
                                      PS3 sees TWO separate devices
```

- Can configfs create two independent gadgets bound to different UDCs? The Deck has one UDC (`/sys/class/udc/` typically shows one entry). Does a USB-C hub expose multiple UDCs?
- If not: could an external microcontroller (Arduino/Teensy/Raspberry Pi Pico) connected to the Deck's second USB port act as the DS3, while the Deck's USB-C does ToyPad?

---

### 🟡 Question 4: The FunctionFS Path for DS3

The expert mentioned that proper DS3 emulation requires FunctionFS (FFS), not configfs hidg. Projects like RosettaPad use FFS to handle ep0 control transfers.

- Is FFS available in SteamOS's kernel? (`CONFIG_USB_F_FS` — Function Filesystem)
- If FFS is available, can it coexist with configfs? (i.e., ToyPad via configfs hidg on one interface, DS3 via FFS on another — back to the composite problem...)
- Is there a known working FFS-based DS3 emulator that runs on Arch Linux (SteamOS's base) that we could adapt?

---

### 🟡 Question 5: Practical Recommendation — DualShock 4 as Separate Controller

The expert's final recommendation: leave the Steam Deck as ToyPad only, use a separate physical controller.

For the specific case of LEGO Dimensions (which requires frequent character swapping on the ToyPad), having the Steam Deck's touchscreen accessible is critical. The `grab()`/`ungrab()` toggle was elegant because it let the Deck serve as both UI and controller without putting down the controller.

**Practical alternatives to explore:**
- Can the Deck's touchscreen remain active as a ToyPad UI even when the Deck's physical controls are being used as a Bluetooth controller for the PS3? (i.e., do evdev and touchscreen input subsystems conflict?)
- Can we use a separate small touchscreen device (phone/tablet) to access the ToyPad web UI at `http://[deck-ip]` while using a standard PS3 controller?
- Is there a way to have the Deck present as BOTH a USB ToyPad AND a USB touchscreen/HID digitizer (so the PS3 ignores the second interface but the Deck's touchscreen still works locally)?

---

## Summary Table

| # | Topic | Core Question |
|---|-------|--------------|
| 1 | Bluetooth DS4 emulation | Can the Deck's Bluetooth present as a DS4 to the PS3 while USB is the ToyPad? (DS4 is simpler than DS3 — no ep0 crypto) |
| 2 | Gadget cycling | Can we "unplug" and rebuild the USB gadget at runtime without restarting the game? Does the game re-scan for ToyPad on hotplug? |
| 3 | Physical USB hub | Would a physical hub between Deck and PS3 let us present two separate USB gadgets? Does the Deck expose multiple UDCs? |
| 4 | FunctionFS viability | Is FFS available in SteamOS kernel? Can it coexist with configfs hidg? |
| 5 | Practical dual-role | Can Deck touchscreen (ToyPad UI) and Deck Bluetooth (controller) operate simultaneously without conflicts? |
