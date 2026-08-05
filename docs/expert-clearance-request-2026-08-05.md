# Final Clearance Request — LD-ToyPad Bridge v9.1

**Date:** 2026-08-05
**To:** Expert Reviewer
**From:** LD-ToyPad Bridge Team

---

## All 5 Risks Mitigated

We implemented every recommendation from your validation. All changes are committed, tagged `v9.1`, and pushed to `github.com/FoggyGoofball/ld-toypad-bridge`.

| # | Risk | Your Recommendation | Our Implementation | File |
|---|------|-------------------|-------------------|------|
| 🔴1 | `bluetoothctl` insufficient | Raw L2CAP sockets required | Removed all `bluetoothctl`. Daemon opens `socket.AF_BLUETOOTH` on PSM 0x11 + 0x13 | `bt-connect-ds3.sh`, `ds3-gamepad-daemon.py` |
| 🔴2 | `/dev/hidg1` doesn't work over BT | Raw Python L2CAP `send()` | Replaced `/dev/hidg1` write with `sock_intr.send()` | `ds3-gamepad-daemon.py` |
| 🔴3 | Missing `0xA1` HID header | 50-byte payload: `0xA1` + 49-byte report | `bytearray([0xA1]) + state.data` prepended | `ds3-gamepad-daemon.py` line 248 |
| 🟡4 | Evdev axis ranges | Confirmed correct; `(v+32768)//256` produces natural IMU calibration at `0x0200` | No change needed | N/A |
| ⚪5 | `system()` in FFS daemon | Acceptable for one-time tool | No change needed | N/A |

---

## Current State

```
v9.1 Architecture:
┌──────────┐    USB (FFS gadget)          ┌──────────┐    BT L2CAP (raw sockets)
│  Deck    │ ──────────────────→ PS3      │  Deck    │ ← ─ ─ ─ ─ →  PS3
│  FFS     │  One-time pairing           │  PSM 0x13│  0xA1 + 49B reports
│  daemon  │  → pairing.json             │  100Hz   │  Player 1
└──────────┘                             │          │
                                         │ USB      │ ────────────→  PS3
                                         │ ToyPad   │   configfs
                                         └──────────┘

Self-installing: zero pre-installed deps beyond BIOS DRD mode.
Entry: curl -sSL .../ldtoypad.sh | sudo bash
```

---

## Pre-Flight Checklist

| Item | Status |
|------|--------|
| `btmgmt public-addr` for MAC spoofing | ✅ `bt-connect-ds3.sh` |
| Raw L2CAP sockets (PSM 0x11 + 0x13) in daemon | ✅ `ds3-gamepad-daemon.py` |
| `0xA1` HID Data\|Input header on every report | ✅ |
| 100Hz write loop (matching PS3 BT timing) | ✅ |
| L4+R4 toggle (`gamepad.grab`/`ungrab`) | ✅ |
| 1:1 Berny23 overlay parity (socket emits, `res.ok` checks, FIFO eviction, reconnect) | ✅ |
| 15 edge-case bugs fixed in overlay JS | ✅ |
| Vanilla SteamOS self-install (pacman, pip, gcc) | ✅ |
| BIOS DRD documented in QUICKSTART.md | ✅ |

---

**Request:** Final clearance to proceed with real-hardware testing on a physical Steam Deck connected to a PS3 running unmodified LEGO Dimensions. Are there any remaining concerns?
