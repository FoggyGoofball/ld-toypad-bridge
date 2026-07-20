# LD-ToyPad Bridge — Path 3 Implementation
## PS3MAPI Injection + NID Hooking Architecture
### Date: 2026-07-19

---

## 1. Executive Summary

The VSH-mode SPRX architecture was proven fundamentally incapable of hooking
game USB functions. The root cause: running inside the VSH process space
prevents access to the game's cellUsbd import table, and the SPRX's polling
loop conflicts with VSH's cellPad input handling.

**Path 3 Solution:** Deliver the SPRX via PS3MAPI runtime injection _after_
the game has fully initialized, then use NID-based memory scanning to locate
the game's cellUsbd function addresses for PowerPC detour hooking.

---

## 2. Architecture Overview

```
┌───────────────┐   HTTP (port 80)    ┌──────────────────┐
│   PS3         │ ◄──────────────── │   PC (Windows)    │
│   webMAN MOD  │   PS3MAPI API     │                   │
│   (always on) │                    │  inject-sprx.js   │
└───────┬───────┘                    │  ┌──────────────┐ │
        │                            │  │ 1. Poll for  │ │
        │ PS3MAPI injection          │  │    game PID   │ │
        │ /ps3mapi_process?pid=...   │  │ 2. Wait 60s  │ │
        │ &load_prx=/dev_hdd0/       │  │ 3. Scan for  │ │
        │ plugins/ldtoypad.sprx      │  │    NIDs      │ │
        ▼                            │  │ 4. Inject    │ │
┌───────────────────┐                │  │    SPRX      │ │
│ GAME (LEGO Dim.)  │                │  └──────────────┘ │
│ ┌───────────────┐ │   UDP:28472    │  ┌──────────────┐ │
│ │ ldtoypad.sprx │ │ ◄───────────  │  │ server.js    │ │
│ │ (injected)    │─┼─►             │  │ (UDP bridge) │ │
│ │               │ │  cellUsbd     │  └──────────────┘ │
│ │ NID scanner   │ │  hooks        │                   │
│ │ → find addrs  │ │  (0x01/0x04)  │  ┌──────────────┐ │
│ │ → install ba  │ │               │  │ Web UI       │ │
│ └───────────────┘ │               │  │ localhost:   │ │
└───────────────────┘               │  │   8080       │ │
                                    │  └──────────────┘ │
                                    └──────────────────┘
```

---

## 3. Files Created/Modified

### New Files

| File | Purpose |
|---|---|
| `ld-toypad-server/scripts/inject-sprx.js` | PS3MAPI game detection + SPRX injection script |
| `docs/HANDOFF-2026-07-19-PATH3-IMPLEMENTATION.md` | This document |

### Modified Files

| File | Changes |
|---|---|
| `sprx-plugin/usb_hooks.c` | Replaced placeholder `find_game_cellusbd_functions()` with real NID-based memory scanner |
| `sprx-plugin/usb_hooks.c` | Added `#include "syscall.h"` for LV2 syscall access |

---

## 4. inject-sprx.js — Injection Script

**Location:** `ld-toypad-server/scripts/inject-sprx.js`

**Workflow:**
1. Connects to PS3MAPI on the PS3 (HTTP port 80)
2. Polls `/ps3mapi_process` every 2s until game PID detected
3. Waits 60s for game to fully initialize (intro cinematics, trophy scan, heap init)
4. Scans game memory for cellUsbd NID import stubs (via PS3MAPI `/read_process`)
5. Injects SPRX via `GET /ps3mapi_process?pid=<PID>&load_prx=<PATH>`

**Usage:**
```bash
# Standard (PS3 IP from ps3-ip.txt)
node inject-sprx.js --verbose

# With explicit IP
node inject-sprx.js --ps3-ip 192.168.0.47 --verbose

# Scan only, no injection
node inject-sprx.js --no-inject --ps3-ip 192.168.0.47

# Custom wait time
node inject-sprx.js --ps3-ip 192.168.0.47 --wait 45
```

**Options:**
| Flag | Default | Description |
|---|---|---|
| `--ps3-ip` | From `ps3-ip.txt` | PS3 IP address |
| `--sprx-path` | `/dev_hdd0/plugins/ldtoypad.sprx` | SPRX path on PS3 |
| `--wait` | `60` | Seconds to wait after game detected |
| `--poll` | `2000` | Poll interval (ms) |
| `--no-inject` | false | Scan only, skip injection |
| `--verbose` | false | Verbose logging |
| `--port` | `80` | PS3MAPI HTTP port |

---

## 5. NID Scanner (C in SPRX)

**Location:** `sprx-plugin/usb_hooks.c` — `find_game_cellusbd_functions()`

**Known cellUsbd NIDs (FNV-1a 32-bit, masked 0x7FFFFFFF):**
| Function | NID |
|---|---|
| `cellUsbdInit` | `0x7F5F00D3` |
| `cellUsbdOpenPipe` | `0x1AB6D80B` |
| `cellUsbdTransfer` | `0x7B4436CE` |
| `cellUsbdClosePipe` | `0x2F82F1A5` |

**Scan Strategy:**
- Searches 5 memory regions (main EBOOT .text/.got2, extended code, PRX areas)
- Looks for PowerPC import stub pattern: `lis r12, nid_high` + `ori r12, r12, nid_low`
- When NID match found, attempts to resolve function address from GOT slot
- Returns success only if all 4 NIDs are found

**Current Limitation (v1):**
The `scan_for_nid()` function correctly identifies NID import stubs but
cannot yet fully resolve the runtime function address from the GOT slot.
The GOT traversal requires walking the PRX module's import descriptor
chain in LV2 memory — a production feature that needs the game's specific
PRX layout data.

**Workaround:** The PC-side injection script (`inject-sprx.js`) attempts
to pre-scan NID locations via PS3MAPI's `/read_process` endpoint and
writes them to `/dev_hdd0/tmp/ld_cellusbd_nids.txt` as a hint file
for the SPRX to read on startup.

---

## 6. Boot Sequence (New)

```
1. Power on PS3 (clean boot, no SPRX in boot_plugins.txt)
2. Start PC server: node server.js --host 0.0.0.0 --http-port 8080
3. Launch LEGO Dimensions on PS3
4. Game plays intro cinematics (~30s)
5. Game reaches "Connect Toy Pad" screen (frozen, awaiting USB)
6. RUN: node inject-sprx.js --ps3-ip 192.168.0.47
   a. Script polls for game PID (2s intervals)
   b. Game PID detected
   c. Waits 60s (full stabilization)
   d. Scans game memory for NIDs
   e. Injects SPRX into game process
7. SPRX module_start runs inside game:
   a. NID scanner locates cellUsbd functions
   b. Detour hooks installed on all 4 functions
   c. Game's USB calls → our hooks → UDP → PC server
8. Toy Pad emulation active
```

---

## 7. Rollback / Cleanup

If you need to revert to VSH-mode:

```bash
# Re-enable boot plugin
# Add to boot_plugins.txt on PS3:
/dev_hdd0/plugins/ldtoypad.sprx

# Re-deploy
.\ftp-deploy.ps1

# Reboot PS3
.\ps3-prep-reboot.ps1
```

---

## 8. Known Limitations & Future Work

1. **GOT address resolution** — `scan_for_nid()` detects NID patterns but
   cannot yet dereference the GOT slot to get the runtime function address.
   This requires implementing a full PRX module table walker via LV2 syscalls.

2. **Game version sensitivity** — NID values are consistent across SDK 3.55+
   but the memory layout (scan regions) may differ between game versions
   (BLUS31548 vs BLES02206) and patch levels.

3. **PS3MAPI dependency** — The injection path requires webMAN MOD with
   PS3MAPI enabled. Not all CFW setups have this.

4. **No game exit cleanup** — When the user exits the game, the SPRX is
   unloaded automatically by the kernel (process termination). This is safe
   but means next launch requires re-injection.

5. **Alternative: LV2 direct injection** — Can use syscall 8/9 to write
   hooks from kernel space into the game, bypassing PS3MAPI entirely.
   More complex but fully automatic.

---

## 9. Quick Start (Testing)

```bash
# Terminal 1: Start server
cd ld-toypad-server
node server.js --host 0.0.0.0 --http-port 8080 --verbose

# Launch LEGO Dimensions on PS3
# Wait for "Connect Toy Pad" screen

# Terminal 2: Inject SPRX
node scripts/inject-sprx.js --ps3-ip 192.168.0.47 --verbose

# Watch both terminals for:
# [Server] Client connected from 192.168.0.47:28472
# [Server] RX type=0x01 zone=1 seq=N
```
