# USB Gadget: DS3 One-Time Pairing → Bluetooth Controller Questions

**Date:** 2026-08-05  
**Project:** LEGO Dimensions ToyPad Emulation for PS3  
**Context:** After confirming the composite gadget approach is dead, exploring whether the Deck can do a ONE-TIME USB DS3 pairing, then switch to Bluetooth for controller + USB for ToyPad.

---

## The Core Idea

Instead of simultaneous composite, use a **pair-once-then-switch** workflow:

```
PHASE 1 (One-time setup):              PHASE 2 (Every play session):
┌──────────┐    USB cable              ┌──────────┐    Bluetooth (DS3)
│  Deck    │ ──────────────→  PS3      │  Deck    │ ← ─ ─ ─ ─ ─ →  PS3
│  spoofs  │  Pair DS3 over USB        │  BT as   │                 Player 1
│  DS3     │  PS3 learns Deck's        │  DS3     │    USB cable
│  gadget  │  BT MAC address           │          │ ──────────────→  PS3
└──────────┘                           │  ToyPad  │   ToyPad
                                       │  gadget  │   (0x0E6F:0x0241)
                                       └──────────┘
```

**The question:** Can we spoof a DS3 over USB JUST ONCE to complete the Bluetooth pairing handshake, then tear down the DS3 gadget and use the Deck's actual Bluetooth radio as the now-paired controller?

---

## How PS3 DS3 Bluetooth Pairing Works (USB Phase)

When you plug a DualShock 3 into a PS3 via USB for the first time, this sequence occurs:

1. **USB Enumeration:** PS3 sees VID `0x054C`, PID `0x0268`. HID descriptor advertises 49-byte reports.

2. **Bluetooth Address Exchange:**
   - PS3 reads the controller's **Bluetooth MAC address** from a USB descriptor or HID feature report. This is how the PS3 learns "what Bluetooth device to look for."
   - PS3 writes its **own Bluetooth MAC address** to the controller via a HID SET_REPORT or control transfer. The controller stores this and will only connect to a host with this address.

3. **Authentication Handshake (ep0 0xF2/0xF5/0xEF):**
   - PS3 sends `GET_REPORT` (0xF2) — controller responds with its 16-byte identifier
   - PS3 sends `SET_REPORT` (0xEF) with configuration data — controller echoes it back
   - PS3 sends `GET_REPORT` (0xF5) — controller responds with status
   - If all correct: controller gets a player slot (LED stops flashing)

4. **PS3 stores pairing:** After successful USB pairing, the PS3 remembers the controller's BT address. The controller remembers the PS3's BT address.

5. **Wireless mode:** User unplugs USB. Controller detects USB disconnect. Controller connects to stored PS3 BT address via Bluetooth HID profile. PS3 recognizes the previously-paired BT address and assigns the same player slot.

---

## Questions

### 🔴 Question 1: Can steps 1-2 (BT Address Exchange) work WITHOUT step 3 (ep0 Authentication)?

Our `usb_f_hid` (configfs hidg) can present the correct VID/PID and HID descriptor (steps 1-2). But it **cannot** handle the ep0 feature report handshake (step 3).

**Critical sub-question:** Does the PS3 perform the Bluetooth address exchange (reading the controller's BT MAC, writing the PS3's BT MAC) **before** or **after** the 0xF2/0xEF/0xF5 authentication sequence?

If the BT address exchange happens **before** ep0 authentication:
- We might be able to complete the pairing (steps 1-2) without passing authentication
- The PS3 would know the Deck's BT MAC and could pair wirelessly later
- The USB controller would just show as "unauthenticated" (flashing LEDs) — but that's fine since we're switching to Bluetooth anyway

If the BT address exchange happens **after** or **during** ep0 authentication:
- configfs hidg can never complete this flow
- We need FunctionFS even for one-time pairing

**Sub-question:** Exactly which USB descriptor/endpoint/report contains the controller's Bluetooth MAC address? Is it:
- A USB string descriptor?
- A HID feature report (requested via control transfer, same ep0 mechanism we can't handle)?
- A value in the HID input report (which configfs CAN send)?

### 🔴 Question 2: Can Linux change the Steam Deck's Bluetooth MAC to match the spoofed DS3?

Assuming we complete USB pairing (somehow), the PS3 stores the controller's Bluetooth MAC — the MAC we reported during USB enumeration. For Bluetooth wireless mode to work, the Deck's actual Bluetooth radio must present the **same** MAC address.

Linux allows changing a Bluetooth adapter's MAC:

```bash
# Example — does this work on Steam Deck's BT chip?
sudo hciconfig hci0 down
sudo hciconfig hci0 addr XX:XX:XX:XX:XX:XX
sudo hciconfig hci0 up
```

**Questions:**
- Does the Steam Deck's Bluetooth chipset (typically Qualcomm/Atheros or Realtek) support MAC address spoofing via `hciconfig` or `btmgmt`?
- Does SteamOS's Bluetooth stack (BlueZ version) allow MAC address changes?
- After changing the BT MAC, can the Deck initiate a connection to the PS3 as a DS3 HID device? What BlueZ profile/plugin handles this?
- Does the PS3 verify anything beyond the MAC address during Bluetooth reconnection? (e.g., link key, PIN, or additional crypto that was exchanged during USB pairing)
- Can we store the PS3's BT MAC (learned during USB pairing) and the Deck's spoofed MAC in a persistent config file, then restore them on boot?

### 🔴 Question 3: The FunctionFS "One-Shot" Approach

The expert confirmed FunctionFS (FFS) can handle ep0 control transfers and is the path used by RosettaPad/droidshock3. 

**Question:** Could we use FunctionFS for a ONE-TIME pairing, then never touch it again?

Architecture:
```bash
# One-time setup script (pair.sh):
# 1. Load FFS kernel module
modprobe usb_f_fs

# 2. Mount FunctionFS
mkdir -p /dev/usb-ffs/ds3
mount -t functionfs ds3 /dev/usb-ffs/ds3

# 3. Run FFS-based DS3 emulator (handles ep0 auth + BT address exchange)
./ds3-ffs-emulator /dev/usb-ffs/ds3

# 4. Bind to UDC — PS3 sees DS3, completes pairing
echo "$UDC" > /sys/kernel/config/usb_gadget/g1/UDC

# 5. PS3 pairs. Emulator captures PS3's BT MAC + Deck's spoofed BT MAC.
# 6. Emulator writes pairing data to ~/.config/ds3-pairing.json
# 7. Unbind, tear down FFS gadget

# Every play session (run-ui.sh):
# 1. Read pairing data from ~/.config/ds3-pairing.json
# 2. Set BT MAC: hciconfig hci0 addr <spoofed-mac>
# 3. Start BlueZ DS3 HID plugin → connects to PS3's stored BT MAC
# 4. Build ToyPad gadget (our proven working setup)
# 5. Start Berny23 emulator
```

**Questions:**
- Is `CONFIG_USB_F_FS` enabled in SteamOS's kernel? (`zcat /proc/config.gz | grep USB_F_FS` or `modinfo usb_f_fs`)
- If not enabled, can it be loaded as a module without a full kernel rebuild? (`modprobe usb_f_fs`)
- Can FunctionFS and configfs coexist? (FFS for the one-time DS3 pairing, configfs for the everyday ToyPad)
- Is there an existing FFS-based DS3 emulator that runs on Arch Linux (SteamOS base)? RosettaPad is Android-based; droidshock3 may be tied to Android's USB stack.
- Once pairing data is saved to disk, can we switch between FFS (DS3) and configfs (ToyPad) by simply binding different gadgets to the UDC? Or does switching require a reboot?

### 🟡 Question 4: DS3 Bluetooth HID Profile on Linux

Assuming we complete USB pairing and spoof the correct BT MAC, the Deck needs to connect to the PS3 via Bluetooth as a DS3 HID device.

**Questions:**
- Does BlueZ (Linux Bluetooth stack) include a DS3 HID profile plugin? The `hid-sony` kernel module handles DS3/DS4 connections — but does it work in the OUTBOUND direction (connecting TO a PS3, rather than accepting connections FROM a DS3)?
- The normal flow is: PS3 → connects to DS3. But in our case, the Deck needs to initiate the connection. Can the Deck's BT radio initiate a connection TO the PS3 as a HID device? Or must the PS3 always be the initiator?
- If the Deck can't initiate: can we trigger the PS3 to reconnect by pressing the PS button on the (now-disconnected) USB gadget before tearing it down? The PS3 might then actively search for the paired BT device.
- Does the Deck's input subsystem (evdev) conflict with BlueZ HID? When BlueZ creates a `/dev/input/` node for the DS3 connection, does it interfere with the Deck's built-in controller evdev nodes?

### 🟡 Question 5: Practical DS4 as Fallback

The user noted DS4 is "out" due to limitations (no PS button, no rumble, no SIXAXIS, incompatible games). However, for LEGO Dimensions specifically:

**Questions:**
- Is LEGO Dimensions one of the "incompatible games" that doesn't recognize DS4 input? Has anyone tested?
- The PS button limitation: can the game be started/navigated using the ToyPad touchscreen UI's connection state (which we control) rather than needing the PS button?
- If DS4 over Bluetooth works for gameplay but not for XMB navigation, could we use the ToyPad's USB presence to trigger the game launch (the PS3 auto-boots the game when the ToyPad is detected)?

---

## Summary Table

| # | Topic | Core Question |
|---|-------|--------------|
| 1 | BT address exchange order | Does PS3 read/write BT MAC addresses BEFORE or AFTER ep0 0xF2/0xF5/0xEF authentication? If before, configfs might suffice for pairing. |
| 2 | Bluetooth MAC spoofing | Can Steam Deck's BT chipset change its MAC address via `hciconfig`/`btmgmt`? Does PS3 verify anything beyond MAC on Bluetooth reconnect? |
| 3 | FunctionFS one-shot | Is `usb_f_fs` in SteamOS kernel? Can FFS handle the one-time pairing, then we switch to configfs forever after? |
| 4 | BlueZ DS3 outbound | Can the Deck initiate a Bluetooth HID connection TO the PS3 (rather than the PS3 connecting to the Deck)? Does `hid-sony` support this direction? |
| 5 | DS4 fallback | Is LEGO Dimensions compatible with DS4 on PS3? Can we work around the PS button limitation using the ToyPad's auto-boot behavior? |
