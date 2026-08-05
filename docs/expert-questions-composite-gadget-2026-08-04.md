# Composite USB Gadget: ToyPad + DualShock 3 — Expert Questions

**Date:** 2026-08-04  
**Project:** LEGO Dimensions ToyPad Emulation for PS3  
**Context:** Extending the proven-working single-device Steam Deck USB gadget (ToyPad only) to a composite gadget (ToyPad + DualShock 3 controller) over a single USB-C cable.

---

## Background: Current Working Setup

We have three proven-working scripts that create a single-interface USB gadget impersonating the LEGO Dimensions ToyPad. All three use the same verified USB descriptor:

**Device-level VID/PID:** `0x0E6F` / `0x0241` (PDP / LEGO READER V2.10)  
**Device strings:** "LEGO READER V2.10", "PDP LIMITED. ", "P.D.P.000000"  
**HID report descriptor:** 29 bytes, 32-byte INPUT / 32-byte OUTPUT (verified against Berny23's `usb_setup_script.sh`)  
**Berny23 emulator:** Reads/writes `/dev/hidg0` (raw HID character device)  
**PS3:** Sees a real LEGO Dimensions ToyPad at boot — no CFW, no EBOOT patches, no game modifications

The HANDOFF document (attached) confirms the core insight: *"The PS3 kernel's USB filter is a hard barrier that cannot be bypassed from userspace. The correct approach is to present valid hardware that passes the filter."*

Our working gadget setup (representative excerpt from `run-ui.sh`):

```bash
GADGET_DIR=/sys/kernel/config/usb_gadget/g1

sudo mkdir -p "$GADGET_DIR" && cd "$GADGET_DIR"

echo 0x0e6f | sudo tee idVendor >/dev/null       # PDP (ToyPad vendor)
echo 0x0241 | sudo tee idProduct >/dev/null       # LEGO READER V2.10
echo 0x0100 | sudo tee bcdDevice >/dev/null
echo 0x0200 | sudo tee bcdUSB >/dev/null

sudo mkdir -p strings/0x409
echo "P.D.P.000000"      | sudo tee strings/0x409/serialnumber >/dev/null
echo "PDP LIMITED. "     | sudo tee strings/0x409/manufacturer >/dev/null
echo "LEGO READER V2.10" | sudo tee strings/0x409/product >/dev/null

sudo mkdir -p configs/c.1/strings/0x409
echo "LEGO READER V2.10" | sudo tee configs/c.1/strings/0x409/configuration >/dev/null
echo 250                 | sudo tee configs/c.1/MaxPower >/dev/null

sudo mkdir -p functions/hid.g0
echo 0  | sudo tee functions/hid.g0/protocol >/dev/null
echo 0  | sudo tee functions/hid.g0/subclass >/dev/null
echo 32 | sudo tee functions/hid.g0/report_length >/dev/null

# Verified 29-byte HID descriptor (matches Berny23's usb_setup_script.sh):
# Usage Page FF00 (vendor), Usage 01, Collection,
#   32-byte INPUT (95 20), 32-byte OUTPUT (91 00)
printf '\x06\x00\xFF\x09\x01\xA1\x01\x19\x01\x29\x20\x15\x00\x26\xFF\x00\x75\x08\x95\x20\x81\x00\x19\x01\x29\x20\x91\x00\xC0' \
  | sudo tee functions/hid.g0/report_desc >/dev/null

sudo ln -sf functions/hid.g0 configs/c.1/

# Bind to UDC
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
echo "$UDC" | sudo tee UDC >/dev/null
sleep 1

sudo chmod 666 /dev/hidg0
```

---

## Expert's Proposed Composite Gadget

The expert proposes replacing the single-interface gadget with a composite gadget containing two HID interfaces on one cable:

```bash
#!/bin/bash
sudo modprobe libcomposite

cd /sys/kernel/config/usb_gadget/
sudo mkdir -p g1
cd g1

# Hub-level VID/PID — generic Linux Foundation composite
echo 0x1d6b | sudo tee idVendor > /dev/null
echo 0x0104 | sudo tee idProduct > /dev/null
echo 0x0100 | sudo tee bcdDevice > /dev/null
echo 0x0200 | sudo tee bcdUSB > /dev/null

sudo mkdir -p strings/0x409
echo "0000000001" | sudo tee strings/0x409/serialnumber > /dev/null
echo "Steam Deck" | sudo tee strings/0x409/manufacturer > /dev/null
echo "ToyPad + DualShock 3 Composite" | sudo tee strings/0x409/product > /dev/null

sudo mkdir -p configs/c.1/strings/0x409
echo "Config 1" | sudo tee configs/c.1/strings/0x409/configuration > /dev/null
echo 250 | sudo tee configs/c.1/MaxPower > /dev/null

# INTERFACE 1: ToyPad (/dev/hidg0)
sudo mkdir -p functions/hid.usb0
echo 0 | sudo tee functions/hid.usb0/protocol > /dev/null
echo 0 | sudo tee functions/hid.usb0/subclass > /dev/null
echo 8 | sudo tee functions/hid.usb0/report_length > /dev/null
echo -ne "\x06\x00\xFF\x09\x01\xA1\x01\x09\x02\x15\x00\x26\xFF\x00\x75\x08\x95\x50\x81\x02\x09\x03\x75\x08\x95\x08\x91\x02\xC0" \
  | sudo tee functions/hid.usb0/report_desc > /dev/null
sudo ln -s functions/hid.usb0 configs/c.1/ 2>/dev/null

# INTERFACE 2: DualShock 3 (/dev/hidg1)
sudo mkdir -p functions/hid.usb1
echo 0 | sudo tee functions/hid.usb1/protocol > /dev/null
echo 0 | sudo tee functions/hid.usb1/subclass > /dev/null
echo 49 | sudo tee functions/hid.usb1/report_length > /dev/null
echo -ne "\x05\x01\x09\x04\xA1\x01\xA1\x02\x85\x01\x75\x08\x95\x01\x15\x00\x26\xFF\x00\x81\x03\x75\x01\x95\x13\x15\x00\x25\x01\x35\x00\x45\x01\x05\x09\x19\x01\x29\x13\x81\x02\x75\x01\x95\x0D\x06\x00\xFF\x81\x03\x15\x00\x26\xFF\x00\x05\x01\x09\x01\xA1\x00\x75\x08\x95\x04\x35\x00\x46\xFF\x00\x09\x30\x09\x31\x09\x32\x09\x35\x81\x02\xC0\x05\x01\x75\x08\x95\x27\x09\x01\x81\x02\x75\x08\x95\x30\x09\x01\x91\x02\x75\x08\x95\x30\x09\x01\xB1\x02\xC0\xC0" \
  | sudo tee functions/hid.usb1/report_desc > /dev/null
sudo ln -s functions/hid.usb1 configs/c.1/ 2>/dev/null

ls /sys/class/udc | sudo tee UDC > /dev/null
sudo chmod 666 /dev/hidg0
sudo chmod 666 /dev/hidg1
```

The Python daemon (`ds3_daemon.py`) uses `evdev` to read Steam Deck hardware inputs and stream 49-byte DS3 HID reports to `/dev/hidg1`:

```python
import evdev
import struct
import time
import threading

PAD_PATH = '/dev/input/by-id/usb-Valve_Software_Steam_Deck_Controller-event-joystick'
GYRO_PATH = '/dev/input/by-id/usb-Valve_Software_Steam_Deck_Controller-event-imu'

gamepad = evdev.InputDevice(PAD_PATH)

ps3_state = bytearray(49)
ps3_state[0] = 0x01  # Report ID
ps3_state[6] = 128   # LX center
ps3_state[7] = 128   # LY center
ps3_state[8] = 128   # RX center
ps3_state[9] = 128   # RY center

is_ps3_mode = False

def write_loop():
    with open('/dev/hidg1', 'wb') as hidg1:
        while True:
            hidg1.write(ps3_state)
            hidg1.flush()
            time.sleep(0.01)  # 100Hz

threading.Thread(target=write_loop, daemon=True).start()

print("Listening for Steam Deck inputs. Press L4 + R4 to toggle PS3 Mode.")

for event in gamepad.read_loop():
    if event.type == evdev.ecodes.EV_KEY:
        if event.code == evdev.ecodes.BTN_GEAR:  # Toggle hotkey
            if event.value == 1:
                is_ps3_mode = not is_ps3_mode
                if is_ps3_mode:
                    gamepad.grab()    # Lock inputs to daemon
                    print("PS3 Controller Mode: ON")
                else:
                    gamepad.ungrab()  # Release back to SteamOS
                    print("Desktop Mode: ON")
                    
    if is_ps3_mode:
        if event.type == evdev.ecodes.EV_KEY:
            if event.code == evdev.ecodes.BTN_SOUTH:  # A button → Cross
                if event.value == 1:
                    ps3_state[3] |= (1 << 1)   # Set Cross bit
                    ps3_state[24] = 255         # Pressure axis
                else:
                    ps3_state[3] &= ~(1 << 1)  # Clear Cross bit
                    ps3_state[24] = 0

        elif event.type == evdev.ecodes.EV_ABS:
            if event.code == evdev.ecodes.ABS_X:
                val = int((event.value + 32768) / 256)
                ps3_state[6] = val
```

---

## Critical Discrepancies & Questions

### 🔴 Question 1: Hub-Level VID/PID vs PS3 Kernel USB Filter

**Discrepancy:** Our working setup uses device-level `0x0E6F:0x0241` (ToyPad vendor/product). The expert's proposal uses hub-level `0x1D6B:0x0104` (Linux Foundation Multifunction Composite Gadget). The PS3 kernel USB filter — confirmed by months of testing — only passes devices matching the real ToyPad's VID/PID to the game.

**Our working descriptor:**
```
idVendor:  0x0E6F  (PDP)
idProduct: 0x0241  (LEGO READER V2.10)
strings:   "LEGO READER V2.10", "PDP LIMITED.", "P.D.P.000000"
```

**Expert's proposed descriptor:**
```
idVendor:  0x1D6B  (Linux Foundation)
idProduct: 0x0104  (Multifunction Composite Gadget)
strings:   "Steam Deck", "ToyPad + DualShock 3 Composite"
```

**Question:** Will the PS3 kernel's USB filter accept `0x1D6B:0x0104` and enumerate the sub-interfaces, or does it filter at the **device descriptor level** (before examining interface descriptors)? Have you tested this exact hub-level VID/PID combination on a PS3? If the filter operates at the device level, the ToyPad interface will never be delivered to the game regardless of what the sub-interface descriptors contain.

---

### 🔴 Question 2: ToyPad HID Report Descriptor Mismatch

**Discrepancy:** The expert's ToyPad HID descriptor is completely different from our verified working descriptor.

**Our verified descriptor** (29 bytes, from Berny23's `usb_setup_script.sh`):
```
06 00 FF     Usage Page FF00 (vendor-defined)
09 01        Usage 01
A1 01        Collection (Application)
19 01        Usage Minimum 01
29 20        Usage Maximum 20
15 00        Logical Minimum 00
26 FF 00     Logical Maximum 00FF
75 08        Report Size 8
95 20        Report Count 32      ← 32-byte INPUT
81 00        Input (Data,Array)
19 01        Usage Minimum 01
29 20        Usage Maximum 20
15 00        Logical Minimum 00
26 FF 00     Logical Maximum 00FF
75 08        Report Size 8
95 20        Report Count 32      ← 32-byte OUTPUT
91 00        Output (Data,Array)
C0           End Collection
```
**Result:** 32-byte INPUT, 32-byte OUTPUT. `report_length = 32`.

**Expert's proposed descriptor** (30 bytes):
```
06 00 FF     Usage Page FF00
09 01        Usage 01
A1 01        Collection
09 02        Usage 02
15 00        Logical Minimum 00
26 FF 00     Logical Maximum 00FF
75 08        Report Size 8
95 50        Report Count 80      ← 80-byte INPUT
81 02        Input (Data,Var)
09 03        Usage 03
75 08        Report Size 8
95 08        Report Count 8       ← 8-byte OUTPUT
91 02        Output (Data,Var)
C0           End Collection
```
**Result:** 80-byte INPUT, 8-byte OUTPUT. `report_length = 8`.

Note: The 80/8 split matches what the HANDOFF document identifies as our **old incorrect assumption** — the assumption we made before discovering Berny23's correct 32/32 descriptor.

**Question:** Can you confirm the correct HID report descriptor for the ToyPad HID interface? Our 32/32 descriptor is verified working with the real game. Does the PS3 game tolerate `report_length=8` vs `report_length=32`? The game sends exactly 32-byte HID OUT reports to the ToyPad.

---

### 🔴 Question 3: DualShock 3 USB Authentication (Control Transfers)

**Discrepancy:** The expert's daemon streams 49-byte HID reports to `/dev/hidg1`, but a real DualShock 3 over USB requires authentication via **USB control transfers** before the PS3 accepts any HID input reports.

The DS3 USB authentication flow:
1. During enumeration, PS3 sends a **control transfer** (bmRequestType=0xA1, bRequest=0x01, wValue=0x03F2) — a feature report request
2. Controller responds with a 16-byte challenge nonce
3. PS3 sends another control transfer with an encrypted response
4. Controller verifies and responds — if correct, controller is "authenticated"
5. Only then does the PS3 begin reading HID input reports

Without the control transfer authentication, the PS3 will enumerate the HID interface but the controller will show as **unauthenticated** (flashing lights, no input accepted).

This is a fundamentally different layer from HID report streaming. The Linux `hidg` (HID Gadget) driver only handles the HID report endpoint — it does not handle USB control transfers. Control transfers go to the gadget's `ep0` (endpoint 0) and require handling at the configfs or kernel driver level.

**Question:** Does the daemon handle the USB control transfer authentication challenge? If not:
- Is there a known way to present as an "already authenticated" controller via configfs?
- Does this require a custom kernel module to intercept ep0 control transfers?
- Would this only work on a PS3 with CFW (which may bypass the authentication check)?
- Have you tested the 148-byte DS3 HID descriptor against a real PS3?

---

### 🟡 Question 4: Complete evdev → DS3 Button Mapping

**Discrepancy:** The expert's example maps only `BTN_SOUTH` → Cross. The complete DS3 49-byte report requires mapping every Steam Deck input.

**DS3 button byte layout (bytes 2-4):**
```
Byte 2: [Select] [L3] [R3] [Start] [Up] [Right] [Down] [Left]
Byte 3: [L2] [R2] [L1] [R1] [Triangle] [Circle] [Cross] [Square]
Byte 4: [PS Button] [unused×7]
```

**Steam Deck evdev codes that need mapping:**
| Steam Deck Input | evdev Code | DS3 Target |
|-----------------|------------|------------|
| D-Pad Up | `BTN_DPAD_UP` / `ABS_HAT0Y` (-1) | Byte 2, bit 4 |
| D-Pad Down | `BTN_DPAD_DOWN` / `ABS_HAT0Y` (+1) | Byte 2, bit 2 |
| D-Pad Left | `BTN_DPAD_LEFT` / `ABS_HAT0X` (-1) | Byte 2, bit 1 |
| D-Pad Right | `BTN_DPAD_RIGHT` / `ABS_HAT0X` (+1) | Byte 2, bit 0 |
| A button | `BTN_SOUTH` | Cross (Byte 3, bit 1) |
| B button | `BTN_EAST` | Circle (Byte 3, bit 2) |
| X button | `BTN_NORTH` | Triangle (Byte 3, bit 4) |
| Y button | `BTN_WEST` | Square (Byte 3, bit 0) |
| L1 bumper | `BTN_TL` | L1 (Byte 3, bit 3) |
| R1 bumper | `BTN_TR` | R1 (Byte 3, bit 5) |
| L2 trigger | `ABS_Z` (analog) | L2 (Byte 3, bit 7 + Byte 18-19 pressure) |
| R2 trigger | `ABS_RZ` (analog) | R2 (Byte 3, bit 6 + Byte 20-21 pressure) |
| L3 stick click | `BTN_THUMBL` | L3 (Byte 2, bit 6) |
| R3 stick click | `BTN_THUMBR` | R3 (Byte 2, bit 5) |
| Select/View | `BTN_SELECT` | Select (Byte 2, bit 7) |
| Start/Menu | `BTN_START` | Start (Byte 2, bit 3) |
| PS button | `BTN_MODE` | PS (Byte 4, bit 0) |
| Left stick X | `ABS_X` (-32768..32767) | Byte 6 (0-255) |
| Left stick Y | `ABS_Y` (-32768..32767) | Byte 7 (0-255) |
| Right stick X | `ABS_RX` (-32768..32767) | Byte 8 (0-255) |
| Right stick Y | `ABS_RY` (-32768..32767) | Byte 9 (0-255) |
| L4 grip | `BTN_TRIGGER_HAPPY1` or `BTN_TRIGGER_HAPPY3` | No DS3 equivalent (use as toggle?) |
| R4 grip | `BTN_TRIGGER_HAPPY2` or `BTN_TRIGGER_HAPPY4` | No DS3 equivalent (use as toggle?) |

**Question:** The expert uses `BTN_GEAR` as the toggle hotkey, but Steam Deck grip buttons are typically `BTN_TRIGGER_HAPPY1` through `BTN_TRIGGER_HAPPY4`. Do you have the complete verified evdev→DS3 mapping for all Steam Deck inputs? Specifically:
- Which evdev codes correspond to L4, R4, L5, R5 on the Steam Deck?
- How are analog L2/R2 triggers mapped (they are analog on Deck, but DS3 expects both digital bits AND pressure bytes)?
- Are the Steam Deck trackpads exposed as separate evdev devices? If so, can they map to DS3 analog sticks?

---

### 🟡 Question 5: Gyroscope / IMU Integration

**Discrepancy:** The expert mentions writing gyro data to DS3 bytes 41-48 but provides no calibration offset, axis mapping, or IMU device path fallback.

**Relevant DS3 bytes for SIXAXIS motion:**
```
Byte 41: Accelerometer X (high byte)
Byte 42: Accelerometer X (low byte)
Byte 43: Accelerometer Y (high byte)
Byte 44: Accelerometer Y (low byte)
Byte 45: Accelerometer Z (high byte)
Byte 46: Accelerometer Z (low byte)
Byte 47: Gyroscope Z (high byte) — yaw
Byte 48: Gyroscope Z (low byte)
```

**Expert's hardcoded path:**
```python
GYRO_PATH = '/dev/input/by-id/usb-Valve_Software_Steam_Deck_Controller-event-imu'
```

This path is specific to LCD Steam Deck models. OLED Decks and certain SteamOS versions may expose the IMU through:
- `/dev/input/by-id/usb-Valve_Software_Steam_Deck_Controller-event-imu` (LCD)
- `/dev/iio:device*` (IIO subsystem, OLED/SteamOS 3.5+)
- Different `by-id` paths on OLED models

**Question:** 
- What is the gyro axis mapping? (Steam Deck IMU raw values → DS3's signed 16-bit range for accel/gyro bytes)
- What calibration offset is required? (DS3 expects ~0x0200 at rest for accel Z due to gravity)
- Does the daemon auto-detect the IMU device node, or should we include fallback detection for both LCD and OLED Decks?

---

### 🟡 Question 6: HID Write Loop Timing & Backpressure

**Discrepancy:** The daemon writes continuously at 100Hz with no state-change detection:

```python
def write_loop():
    with open('/dev/hidg1', 'wb') as hidg1:
        while True:
            hidg1.write(ps3_state)   # Writes even when nothing changed
            hidg1.flush()
            time.sleep(0.01)
```

The PS3 polls the HID interrupt endpoint at a fixed rate (defined by `bInterval` in the endpoint descriptor). The USB HID gadget driver buffers writes. If the daemon writes faster than the PS3 polls, the kernel buffer may fill. Conversely, if the daemon writes slower than the PS3 polls, the PS3 may see stale data.

**Question:**
- Is 100Hz confirmed as the PS3's DS3 HID polling rate over USB?
- Should the write loop check for state changes before writing (to avoid flooding the kernel buffer with duplicate reports)?
- Should we synchronize to the actual interrupt endpoint polling interval from the HID descriptor rather than using a fixed `time.sleep(0.01)`?

---

## Summary of All Questions

| # | Severity | Topic | Core Concern |
|---|----------|-------|-------------|
| 1 | 🔴 Critical | Hub-level VID/PID `0x1D6B:0x0104` | Will PS3 kernel USB filter pass a generic Linux Foundation composite device, or does it filter at device descriptor level? |
| 2 | 🔴 Critical | ToyPad HID descriptor (80/8 vs verified 32/32) | Does the game accept report_length=8 instead of 32? Our 32-byte descriptor is the verified working one. |
| 3 | 🔴 Critical | DS3 USB control transfer authentication | How is the challenge-response authentication handled? The hidg driver doesn't intercept ep0 control transfers. Does this require CFW? |
| 4 | 🟡 Significant | Incomplete evdev→DS3 button mapping | Only BTN_SOUTH→Cross shown. Need full mapping for all buttons, sticks, triggers, and grip buttons. |
| 5 | 🟡 Significant | Gyro/IMU calibration and OLED compatibility | Hardcoded LCD-only IMU path. No axis mapping, no calibration offset. |
| 6 | 🟡 Significant | Write loop timing and backpressure | 100Hz fixed writes with no change detection. Should match actual endpoint polling interval. |

---

## Attachments

- `docs/HANDOFF-FINAL-2026-08-03.md` — Full handoff report explaining why hardware impersonation works and software patching failed, including the verified ToyPad USB descriptor
- `deck/run.sh` — Working vanilla Berny23 setup script (single ToyPad interface)
- `deck/run-ui.sh` — Working custom UI overlay setup script (single ToyPad interface)
- `deck/deck_toypad.sh` — Legacy setup script (single ToyPad interface)
