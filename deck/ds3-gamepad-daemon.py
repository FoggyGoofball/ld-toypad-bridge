#!/usr/bin/env python3
"""
ds3-gamepad-daemon.py — Steam Deck evdev → DualShock 3 Bluetooth HID

Reads Steam Deck hardware inputs (buttons, joysticks, gyro) via evdev and
translates them to 49-byte DS3 HID reports.  Sends reports over Bluetooth
to a previously-paired PS3 (via /dev/hidg1 or directly via L2CAP).

TOGGLE MODE (L4 + R4):
  - gamepad.grab()   → Deck controls go to PS3 (Desktop cursor locked)
  - gamepad.ungrab() → Deck controls return to SteamOS (touch ToyPad UI)

Usage:
  sudo python3 ds3-gamepad-daemon.py

Prerequisites:
  pip install evdev
  Option 3 (pair DS3) must have been run first.
"""

import evdev
import struct
import time
import threading
import os
import json
import subprocess
import sys
from evdev import InputDevice, ecodes, categorize

# ── Configuration ───────────────────────────────────────────────
PAIRING_FILE = os.path.expanduser("~/.config/ld-toypad/ds3-pairing.json")
BT_DEVICE = "/dev/hidg1"    # Bluetooth HID gadget device (if using configfs BT HID)
                            # Set to None to use raw L2CAP socket instead

# Steam Deck evdev device paths — auto-detected if empty
PAD_PATH = ""
GYRO_PATH = ""

# ── DS3 49-byte report structure ────────────────────────────────
# Byte 0:    Report ID (0x01)
# Byte 1:    Reserved (0x00)
# Byte 2:    Buttons1 [Select L3 R3 Start Up Right Down Left]
# Byte 3:    Buttons2 [L2 R2 L1 R1 Triangle Circle Cross Square]
# Byte 4:    PS Button
# Byte 5:    Reserved
# Byte 6-9:  Left Stick X/Y, Right Stick X/Y (0-255, center=128)
# Byte 10-13: D-pad pressure (0xFF = pressed)
# Byte 14-17: Reserved
# Byte 18-19: L2/R2 analog (0-255)
# Byte 20-25: Face button pressure (0xFF = pressed)
# Byte 26-28: Reserved
# Byte 29:   Plugged status (0x02=USB, 0x03=unplugged)
# Byte 30:   Battery (0x05=full)
# Byte 31:   Connection (0x16=BT, 0x14=BT+rumble)
# Byte 32-39: Reserved / unknown fixed
# Byte 40-47: Accel X/Y/Z, Gyro Z (10-bit LE)
# Byte 48:   Final (0x02)

class DS3State:
    """Mutable 49-byte DS3 HID report."""
    def __init__(self):
        self.data = bytearray(49)
        self.data[0] = 0x01   # Report ID
        self.data[6] = 128    # LX center
        self.data[7] = 128    # LY center
        self.data[8] = 128    # RX center
        self.data[9] = 128    # RY center
        self.data[29] = 0x03  # Unplugged (BT mode)
        self.data[30] = 0x05  # Battery full
        self.data[31] = 0x16  # BT connection (0x14 with rumble)
        self.data[36] = 0x33  # Fixed
        self.data[37] = 0x04  # Fixed
        self.data[38] = 0x77  # Fixed
        self.data[39] = 0x01  # Fixed
        self.data[48] = 0x02  # Final byte

    def set_button(self, byte_idx, bit, pressed):
        if pressed:
            self.data[byte_idx] |= (1 << bit)
        else:
            self.data[byte_idx] &= ~(1 << bit)

    def set_axis(self, byte_idx, value):
        """Convert signed 16-bit (-32768..32767) to unsigned 8-bit (0-255)."""
        self.data[byte_idx] = int((value + 32768) / 256) & 0xFF

    def set_pressure(self, byte_idx, pressed):
        self.data[byte_idx] = 0xFF if pressed else 0x00

    def set_accel(self, base_byte, value):
        """10-bit unsigned LE."""
        self.data[base_byte] = value & 0xFF
        self.data[base_byte + 1] = (value >> 8) & 0x03

    def bytes(self):
        return bytes(self.data)

# ── Button mapping: evdev code → (DS3 byte, bit) ────────────────
BUTTON_MAP = {
    # Buttons1 (Byte 2)
    ecodes.BTN_SELECT:       (2, 0),   # Select
    ecodes.BTN_THUMBL:       (2, 1),   # L3
    ecodes.BTN_THUMBR:       (2, 2),   # R3
    ecodes.BTN_START:        (2, 3),   # Start
    ecodes.BTN_DPAD_UP:      (2, 4),   # D-pad Up
    ecodes.BTN_DPAD_RIGHT:   (2, 5),   # D-pad Right
    ecodes.BTN_DPAD_DOWN:    (2, 6),   # D-pad Down
    ecodes.BTN_DPAD_LEFT:    (2, 7),   # D-pad Left

    # Buttons2 (Byte 3)
    ecodes.BTN_TL2:          (3, 0),   # L2 digital (also analog byte 18)
    ecodes.BTN_TR2:          (3, 1),   # R2 digital (also analog byte 19)
    ecodes.BTN_TL:           (3, 2),   # L1
    ecodes.BTN_TR:           (3, 3),   # R1
    ecodes.BTN_NORTH:        (3, 4),   # Y → Triangle
    ecodes.BTN_EAST:         (3, 5),   # B → Circle
    ecodes.BTN_SOUTH:        (3, 6),   # A → Cross
    ecodes.BTN_WEST:         (3, 7),   # X → Square

    # PS Button (Byte 4)
    # PS button — mapped to L5 + R5 grip buttons (not BTN_MODE)
    # L5 triggers PS button press, R5 also triggers PS button press
}

# Pressure byte indices for face buttons
PRESSURE_MAP = {
    ecodes.BTN_NORTH: 22,    # Triangle pressure
    ecodes.BTN_EAST:  23,    # Circle pressure
    ecodes.BTN_SOUTH: 24,    # Cross pressure
    ecodes.BTN_WEST:  25,    # Square pressure
    ecodes.BTN_TL:    20,    # L1 pressure
    ecodes.BTN_TR:    21,    # R1 pressure
}

# Axis mapping: evdev code → DS3 byte
AXIS_MAP = {
    ecodes.ABS_X:  6,    # Left stick X
    ecodes.ABS_Y:  7,    # Left stick Y
    ecodes.ABS_RX: 8,    # Right stick X
    ecodes.ABS_RY: 9,    # Right stick Y
    ecodes.ABS_Z:  18,   # L2 analog
    ecodes.ABS_RZ: 19,   # R2 analog
}

# Grip button roles:
#   L4 (BTN_TRIGGER_HAPPY1) + R4 (BTN_TRIGGER_HAPPY2) = toggle hotkey
#   L5 (BTN_TRIGGER_HAPPY3) + R5 (BTN_TRIGGER_HAPPY4) = PS button
TOGGLE_L = ecodes.BTN_TRIGGER_HAPPY1   # L4
TOGGLE_R = ecodes.BTN_TRIGGER_HAPPY2   # R4
PS_BTN_L = ecodes.BTN_TRIGGER_HAPPY3   # L5 → PS button
PS_BTN_R = ecodes.BTN_TRIGGER_HAPPY4   # R5 → PS button

# Trackpad evdev codes — explicitly ignored in DS3 mode
# (Steam Deck trackpads share right-stick axes; user should disable
#  trackpad→stick mapping in Steam Input for clean separation)
TRACKPAD_IGNORE = {
    # These may appear if trackpads are exposed as separate axes
}


def auto_detect_devices():
    """Find Steam Deck controller and IMU evdev nodes."""
    pad = None
    gyro = None

    for dev_path in evdev.list_devices():
        try:
            dev = InputDevice(dev_path)
            name = dev.name.lower()

            if "steam deck" in name or "valve" in name:
                caps = dev.capabilities().get(ecodes.EV_ABS, [])
                has_sticks = any(c[0] in (ecodes.ABS_X, ecodes.ABS_RX) for c in caps)

                if "imu" in name:
                    gyro = dev_path
                elif has_sticks and "joystick" in name:
                    pad = dev_path
                elif has_sticks and pad is None:
                    pad = dev_path

        except (PermissionError, OSError):
            continue

    # Fallback: try common paths
    if pad is None:
        for path in [
            "/dev/input/by-id/usb-Valve_Software_Steam_Deck_Controller-event-joystick",
            "/dev/input/by-id/usb-Valve_Software_Steam_Controller-event-joystick",
        ]:
            if os.path.exists(path):
                pad = path
                break

    if gyro is None:
        for path in [
            "/dev/input/by-id/usb-Valve_Software_Steam_Deck_Controller-event-imu",
        ]:
            if os.path.exists(path):
                gyro = path
                break

    return pad, gyro


def main():
    global PAD_PATH, GYRO_PATH

    if os.geteuid() != 0:
        print("Must run as root (sudo).")
        sys.exit(1)

    # Auto-detect devices
    pad_path, gyro_path = auto_detect_devices()
    if pad_path is None:
        print("ERROR: Could not find Steam Deck controller evdev node.")
        print("  Is the controller enabled in Steam Input?")
        print("  Try: ls /dev/input/by-id/ | grep -i valve")
        sys.exit(1)

    PAD_PATH = pad_path
    GYRO_PATH = gyro_path

    print(f"  Gamepad: {PAD_PATH}")
    print(f"  Gyro:    {GYRO_PATH or 'Not detected'}")

    # Open gamepad
    gamepad = InputDevice(PAD_PATH)
    print(f"  Device:  {gamepad.name}")

    # State
    state = DS3State()
    is_ps3_mode = False
    l4_pressed = False
    r4_pressed = False

    # ── Write thread ────────────────────────────────────────────
    def write_loop():
        """Stream DS3 reports to Bluetooth HID device at ~125Hz."""
        if BT_DEVICE and os.path.exists(BT_DEVICE):
            fd = open(BT_DEVICE, "wb")
        else:
            print("  No BT HID device. Reports not sent (debug mode).")
            fd = None

        try:
            while True:
                if fd:
                    fd.write(state.bytes())
                    fd.flush()
                time.sleep(0.008)  # ~125Hz
        finally:
            if fd:
                fd.close()

    threading.Thread(target=write_loop, daemon=True).start()

    # ── Gyro thread ─────────────────────────────────────────────
    gyro_device = None
    if GYRO_PATH and os.path.exists(GYRO_PATH):
        try:
            gyro_device = InputDevice(GYRO_PATH)
            print(f"  Gyro device: {gyro_device.name}")
        except Exception as e:
            print(f"  Gyro init failed: {e}")

    def gyro_loop():
        """Read IMU and update DS3 accel/gyro bytes."""
        if gyro_device is None:
            return
        try:
            for event in gyro_device.read_loop():
                if event.type == ecodes.EV_ABS:
                    val = event.value
                    # Steam Deck IMU: raw 16-bit → 10-bit DS3 range (0-1023)
                    mapped = int((val + 32768) / 64) & 0x3FF
                    if event.code == ecodes.ABS_X:
                        state.set_accel(40, mapped)  # Accel X
                    elif event.code == ecodes.ABS_Y:
                        state.set_accel(42, mapped)  # Accel Y
                    elif event.code == ecodes.ABS_Z:
                        state.set_accel(44, mapped)  # Accel Z
                    elif event.code == ecodes.ABS_RX:
                        state.set_accel(46, mapped)  # Gyro Z
        except Exception:
            pass

    if gyro_device:
        threading.Thread(target=gyro_loop, daemon=True).start()

    # ── Main event loop ─────────────────────────────────────────
    print(f"\n{'='*50}")
    print(f"  DS3 Gamepad Daemon running.")
    print(f"  L4+R4  = toggle PS3 / Desktop mode")
    print(f"  L5+R5  = PS button")
    print(f"  Trackpads: unused in DS3 mode")
    print(f"  Current: {'PS3 CONTROLLER' if is_ps3_mode else 'DESKTOP (ToyPad UI)'}")
    print(f"  Ctrl+C to exit.")
    print(f"{'='*50}\n")

    try:
        for event in gamepad.read_loop():
            # ── Toggle hotkey: L4 + R4 simultaneously ────────────
            # Only fires when BOTH grip buttons are held down on the
            # exact key event that completes the pair (avoids toggle
            # on unrelated key events when grips happen to be held)
            if event.type == ecodes.EV_KEY:
                is_toggle_key = (event.code == TOGGLE_L or event.code == TOGGLE_R)

                if event.code == TOGGLE_L:
                    l4_pressed = (event.value == 1)
                elif event.code == TOGGLE_R:
                    r4_pressed = (event.value == 1)

                # Toggle ONLY on L4 or R4 press events, and ONLY when both are held
                if is_toggle_key and l4_pressed and r4_pressed and event.value == 1:
                    is_ps3_mode = not is_ps3_mode
                    if is_ps3_mode:
                        gamepad.grab()
                        print(">>> PS3 CONTROLLER MODE (L4+R4) <<<")
                    else:
                        gamepad.ungrab()
                        print(">>> DESKTOP MODE (L4+R4) <<<")
                    continue  # Don't forward toggle keys to DS3

            # ── PS3 mode: translate inputs ──────────────────────
            if is_ps3_mode:
                if event.type == ecodes.EV_KEY:
                    # PS button via L5 or R5 grip buttons
                    if event.code == PS_BTN_L or event.code == PS_BTN_R:
                        state.set_button(4, 0, event.value == 1)  # Byte 4, bit 0 = PS
                        continue

                    # L4/R4 are toggle-only, never forwarded to DS3
                    if event.code == TOGGLE_L or event.code == TOGGLE_R:
                        continue

                    # Buttons
                    if event.code in BUTTON_MAP:
                        byte_idx, bit = BUTTON_MAP[event.code]
                        state.set_button(byte_idx, bit, event.value == 1)

                    # Pressure
                    if event.code in PRESSURE_MAP:
                        state.set_pressure(PRESSURE_MAP[event.code], event.value == 1)

                    # D-pad pressure
                    if event.code in (ecodes.BTN_DPAD_UP, ecodes.BTN_DPAD_DOWN,
                                      ecodes.BTN_DPAD_LEFT, ecodes.BTN_DPAD_RIGHT):
                        dpad_map = {
                            ecodes.BTN_DPAD_UP: 10, ecodes.BTN_DPAD_DOWN: 12,
                            ecodes.BTN_DPAD_LEFT: 13, ecodes.BTN_DPAD_RIGHT: 11,
                        }
                        state.set_pressure(dpad_map[event.code], event.value == 1)

                elif event.type == ecodes.EV_ABS:
                    # Trackpads: unused in DS3 mode (skip right-stick axes if
                    # trackpad is the source — user should disable trackpad→stick
                    # mapping in Steam Input for clean separation)
                    # Analog axes (physical sticks + L2/R2 triggers)
                    if event.code in AXIS_MAP:
                        state.set_axis(AXIS_MAP[event.code], event.value)

    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        if is_ps3_mode:
            try:
                gamepad.ungrab()
            except Exception:
                pass


if __name__ == "__main__":
    main()
