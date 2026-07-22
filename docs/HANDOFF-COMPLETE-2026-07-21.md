# LD-ToyPad Bridge — Complete Handoff for New Agent
## Date: 2026-07-21 (Updated 22:00)

---

## 1. What Is This Project?

The LD-ToyPad Bridge is a **PC↔PS3 bridge** that intercepts USB calls in LEGO Dimensions running on a PS3 (with webMAN MOD / Cobra CFW) and redirects them to a Node.js server on the PC. This allows using **USB toy pads connected to the PC** instead of a physical PS3 USB toy pad.

**The PC server emulates the toy pad protocol** (`toypad-protocol.js`) and communicates with `virtual-toys.js` for interactive web UI support.

---

## 2. Architecture Overview

```
┌──────────────────────┐    UDP/28472     ┌──────────────────────┐
│   PS3 (LEGO DIM.)   │  ◄──────────►   │   PC (Node.js)       │
│                      │                  │                      │
│  Game calls:         │  packets:        │  server.js           │
│  cellUsbdInit()      │  RX/TX beacons  │  toypad-protocol.js  │
│  cellUsbdOpenPipe()  │  keepalive      │  virtual-toys.js     │
│  cellUsbdTransfer()  │  heartbeat      │                      │
│  cellUsbdClosePipe() │                  │  Web UI (app.js)    │
│                      │                  │                      │
│  SPRX hooks these    │                  │  inject-sprx.js      │
│  functions via       │                  │  (PC→webMAN inject)  │
│  trampoline preambles│                  │                      │
└──────────────────────┘                  └──────────────────────┘
```

---

## 3. Two-Phase Hook Architecture

### PHASE 1 (SPRX-side, runs in game process):
1. `module_start` → worker thread created
2. `network_init()` → UDP socket + bind (+ retry loop for socket)
3. `network_wait_ready()` → wait for interface
4. `usb_hook_init()` → allocate trampoline page, write IPC file
5. Main loop: recv, heartbeat++, probe, keepalive, sleep 50ms

### PHASE 2 (Node.js-side, inject-sprx.js):
1. Poll `GET /ps3mapi.ps3?PROCESS%20GETCURRENTPID` → get game PID
2. Wait for stabilization period (30s)
3. `MODULE UNLOAD` old PRX
4. `MODULE LOAD` new SPRX
5. Poll `GET /dev_hdd0/tmp/ld_hooks_ready.txt` → get addresses
6. For each of 4 functions: build preamble, `MEMORY SET` via PS3MAPI
7. Game now routes cellUsbd calls through hooks

---

## 4. Current Status (CRITICAL: Read Carefully)

### ✅ What Works
- **Build pipeline**: `make clean && make` in WSL produces `build/ldtoypad.sprx` (18,528 bytes)
  - 8 source files compile and link
  - oscetool 0.9.2 signs with `-5 APP` (NOT `-5 ISLAND` — that flag causes "Invalid SELF type")
- **FTP deploy**: `ftp-deploy.ps1` uploads SPRX to `/dev_hdd0/plugins/` and registers in `boot_plugins.txt`
- **webMAN connectivity**: inject-sprx.js can detect webMAN and get game PID
- **SPRX injection**: `MODULE LOAD 0xPID /dev_hdd0/plugins/ldtoypad.sprx` returns `{"code": 200, "status": "OK"}`
- **Network init (SPRX side)**: socket()+bind() succeeds on attempt 1 in current build
  - UDP bound on port 28472
  - Startup beacon salvo sent
- **VSH Guard**: SPRX detects when loaded in VSH (XMB) context and unloads immediately
  - "VSH GUARD: detected VSH context — unloading immediately"

### ❌ What's Blocking (Current Bug)
**The NID scanner inside the SPRX (`usb_hooks.c`) cannot find the game's cellUsbd functions.**

Evidence from `ldtoypad_debug.log`:
```
[LDTP] [USB] Initializing USB hooks
[LDTP] [USB] Scanning game memory for cellUsbd NIDs...
```
...then nothing. No IPC file written. No addresses found.

**Root Cause:** `sys_prx_get_module_info(NULL, 0, &info)` with `NULL` for the module ID returns the **SPRX's own segments**, not the game's `.text` segment. The game's cellUsbd functions are in the game's ELF, not in the SPRX.

**The NID scanner searches within the SPRX's own memory range and finds nothing.**

### 📋 Log Analysis
The `ldtoypad_debug.log` (accessible via FTP at `/dev_hdd0/plugins/ldtoypad_debug.log`) shows:
```
[LDTP] [NET] network_init(port=28472) ENTRY
[LDTP] [NET] socket()+bind() succeeded on attempt 1
[LDTP] [NET] UDP ready on port 28472
[LDTP] [MAIN] Network interface ready
[LDTP] [USB] Initializing USB hooks
[LDTP] [USB] Scanning game memory for cellUsbd NIDs...
```
Network works. USB hook init begins. NID scan starts but returns nothing.

The `ldtoypad_boot.log` (plain-text, less verbose) shows:
```
=== ldtoypad Full Integration: module_start ===
Game process confirmed — creating worker thread...
OK: debug_init()
OK: network_init(28472)
OK: network_wait_ready()
OK: network_set_server(192.168.0.17:28472)
```

---

## 5. Proposed Fix: PC-Side NID Scanning

**Strategy:** Move the NID scanning from the SPRX to the injector (Node.js side). The SPRX will:
1. Allocate the 64KB trampoline page via `sys_memory_allocate()`
2. Build the trampolines (copy original instructions, build branch-back)
3. Write the IPC file with the trampoline page address
4. The PC injector will:
   - Read the game ELF from PS3 FTP (`/dev_hdd0/disc/` or `/dev_hdd0/game/`)
   - Parse the ELF to find cellUsbd NID stubs
   - Calculate function addresses (base + offset)
   - Build 4-instruction preambles (lis/ori/mtctr/bctr)
   - Write preambles via `MEMORY SET` (Ring 0 bypasses R-X protection)

### SPRX Changes Needed:
- `usb_hooks.c`: Remove NID scanner, just allocate trampoline page
- `main.c`: Call simplified `usb_hook_init()` that allocates page + builds trampolines + writes IPC
- Remove `sys_prx_get_module_info` calls (no longer needed in SPRX)

### Injector Changes Needed:
- Add game ELF parser (read `.text` section, scan for NID stubs)
- Add function address calculation (game base + section offset + NID offset)
- Build preambles (same 16-byte lis/ori/mtctr/bctr as current)
- Write preambles via `MEMORY SET`

---

## 6. Blocking Bugs — Investigation Tasks for Next Agent

### Bug A: NID scanner searches wrong process memory

**Symptom:** The SPRX boots network fine, reaches `usb_hook_init()`, logs "Scanning game memory for cellUsbd NIDs...", then writes nothing — no IPC file, no hook addresses.

**Probable Root Cause:** `sys_prx_get_module_info(NULL, 0, &info)` with `module_id = NULL` returns the ***SPRX's own module segments***, not the game process's `.text` segment. The SPRX's text section (a few KB of compiled plugin) doesn't contain any cellUsbd NID stubs — those are in the game's ELF (megaton_sprx_xxxxx).

**Task: Implement PC-side function address discovery.** Delete or bypass the broken SPRX NID scanner. Instead:

1. **SPRX side:** Simplify `usb_hook_init()` to only:
   - Call `sys_memory_allocate(0x10000, 0, &trampoline_page)` to allocate 64KB R-W-X page
   - Build the 4 × 32-byte trampolines at known offsets in the page (copy 4 original instructions, build branch-back)
   - Write IPC file with trampoline addresses only (no target addresses yet)
   - Read heartbeat pointer from offset 128 in trampoline page

2. **Injector side (Node.js):** Add a game ELF parser that:
   - Discovers the LEGO Dimensions ELF path (try: `/dev_dvd/PS3_GAME/USRDIR/`, `/dev_hdd0/game/BLUS31548/`, or process memory dump)
   - Downloads it via FTP (`ftp://mike:mike@192.168.0.47/dev_hdd0/...`)
   - Parses the ELF header, locates `.text` section (sh_offset + sh_addr)
   - Scans for cellUsbd NID stubs (the 4 known NID byte sequences from `usb_hooks.c`)
   - For each NID found, reads the branch instruction that follows it to dereference the function pointer → gets the **actual function address**
   - Calculates target addresses as: `game_base + section_offset + nid_offset_within_section`
   - Builds 4-instruction preambles (lis/ori/mtctr/bctr) pointing to trampolines
   - Writes preambles via `GET /ps3mapi.ps3?MEMORY%20SET%200xPID%200xADDR%20HEXDATA`

**Resources to check:**
- `sprx-plugin/usb_hooks.c` — Current (broken) NID scanner, can salvage NID constants
- `sprx-plugin/hook.h` — Opcode building macros (LIS, ORI, MTCTR, BCTR)
- `ld-toypad-server/scripts/inject-sprx.js` — Already does preamble writing, just needs address discovery

### Bug B: Potential `recvfrom` blocking → `module_stop` hang

**Symptom (latent — not yet observed because Bug A blocks first):** When MODULE UNLOAD is called, `module_stop()` closes socket and joins thread. If `recvfrom()` is blocking (non-blocking flag not actually applied), the thread never evaluates `g_shutdown` and never exits. `sys_ppu_thread_join()` then hangs forever.

**Task: Verify and harden non-blocking socket setup in `network.c`.**

1. **Check `network.c`:** Find where `SO_NBIO` / `O_NONBLOCK` is set. Is `accept_flag = 1; setsockopt(s, 0xFFFF, 0x2001, ...)` actually applying the non-blocking flag? (The setsockopt level 0xFFFF = SOL_SOCKET, optname 0x2001 = SO_NBIO)

2. **Verify approach:** Add a debug log after setsockopt that tries `recvfrom()` with a 0-timeout poll first — if it doesn't return `EAGAIN`/`EWOULDBLOCK` immediately, the non-blocking flag isn't working.

3. **Fallback fix:** If `setsockopt(SO_NBIO)` doesn't work reliably, switch to `recvfrom()` with `MSG_DONTWAIT` flag (if available on CellOS) or implement a poll-based approach with `sys_net_poll()` / `select()` before each `recvfrom()` call.

4. **Relevant code:** `sprx-plugin/network.c` — `network_init()` (sets up socket) and `network_recv_packet()` (calls recvfrom). Also `sprx-plugin/main.c` — `module_stop()` (closes socket, joins thread).

### Bug C: sendto errors pollute debug log

**Symptom:** When the bridge server isn't running, the debug log fills with `[LDTP_ERROR] [NET] sendto failed: -2147417535` endlessly — tens of thousands of lines, eating flash writes.

**Task: Add error-rate limiting.** In the worker loop, after N consecutive sendto failures, either:
- Exponential backoff (e.g., double sleep interval each time, cap at 5s)
- Skip sendto entirely after 10 consecutive failures, retry once every 60s
- Or: check if server is reachable first via a quick getsockname/connect probe

**Relevant code:** `sprx-plugin/network.c` — the beacon/keepalive send loop.

### Bug D: webserver not restarted after crash

**Observation:** The Node.js bridge server is started via `start /B node server.js` in a cmd window. If it crashes, there's no restart mechanism.

**Task:** Add a simple watchdog or use `node --watch` / `nodemon` to auto-restart. Or document in the startup scripts that the user should check server-output.txt for crash logs.

---

## 7. Build, Sign & Deploy Pipeline (Complete Reference)

### Prerequisites
- **SDK**: Sony PS3 DUPLEX (SN Systems) at `/mnt/c/usr/local/cell/`
- **Signing**: `oscetool` at `$HOME/oscetool-src/oscetool`
- **WSL**: Windows Subsystem for Linux (any distro)
- **PS3**: Connected to network at `192.168.0.47`, webMAN MOD 1.47.48c+

### Step 1: Rebuild the SPRX
```bash
# In WSL (Git Bash or cmd with wsl prefix):
wsl bash -c "cd /mnt/c/Users/Admin/source/repos/dimensions\ plugin/sprx-plugin && make clean && make"
```

This will:
1. Compile all `.c` and `.s` files in `sprx-plugin/`
2. Link into `build/ldtoypad.prx` (113,816 bytes)
3. Sign via oscetool into `build/ldtoypad.sprx` (18,528 bytes)

**Key source files:**
| File | Purpose |
|------|---------|
| `main.c` | module_start/stop, worker thread, init chain |
| `network.c` | UDP socket, bind, retry, send/recv, keepalive |
| `network.h` | State struct, packet types, extern `g_net` |
| `usb_hooks.c` | NID scanner (BROKEN), trampoline alloc, IPC file |
| `usb_hooks.h` | Hook state struct, function declarations |
| `hook.h` | PowerPC opcode builders (lis, ori, mtctr, bctr) |
| `toc_trampoline.s` | Assembly TOC wrappers (5 call_original stubs) |
| `toc_trampoline_c.c` | OPD definitions linking assembly to C |
| `debug.c` / `debug.h` | CellFS debug logging to HDD |
| `toypad_state.c` / `toypad_state.h` | Game state machine |
| `compat.c` | SDK compatibility (IPPROTO_UDP) |
| `Makefile` | Build rules, oscetool signing |

### Step 2: Deploy to PS3
```powershell
.\ftp-deploy.ps1
```
Uploads `sprx-plugin/build/ldtoypad.sprx` to `/dev_hdd0/plugins/ldtoypad.sprx` via FTP (mike/mike).

Optionally registers in `/dev_hdd0/boot_plugins.txt` (Cobra auto-load).

### Step 3: Start Bridge Server
```bash
node ld-toypad-server/server.js
```
Listens on UDP port 28472 for PS3 packets.

### Step 4: Boot LEGO Dimensions on PS3
Wait until "Connect Toy Pad" screen is visible.

### Step 5: Run Injector
```bash
node ld-toypad-server/scripts/inject-sprx.js --verbose --wait 30
```

What happens:
1. Polls webMAN for game PID (every 2s, up to 80 attempts)
2. Waits 30s for game stabilization
3. Unloads previous PRX (if exists, 500ms delay)
4. `MODULE LOAD` → SPRX loaded into game process
5. Polls for `ld_hooks_ready.txt` IPC file (every 1s, up to 80 attempts)
6. Parses addresses from IPC file
7. Writes preambles via `MEMORY SET`

### Step 6: Check Boot Log
```bash
curl -u mike:mike ftp://192.168.0.47/dev_hdd0/plugins/ldtoypad_boot.log
```
Or for verbose debug:
```bash
curl -u mike:mike ftp://192.168.0.47/dev_hdd0/plugins/ldtoypad_debug.log
```

---

## 8. Critical File: inject-sprx.js

**Location:** `ld-toypad-server/scripts/inject-sprx.js`

Key behaviors:
- Parses `json.response` (NOT `json.result`)
- Handles 64-bit PID format with leading zeros (`0x0000000001010200`)
- Unloads previous PRX before loading new one
- Retries injection once on failure
- Builds preambles as 16-byte buffers (lis/ori/mtctr/bctr)
- Writes via `GET /ps3mapi.ps3?MEMORY%20SET%200xPID%200xADDR%20HEX`

Expected success output:
```
✓ webMAN detected
✓ Game detected! PID=0x1010200 (16843264)
✓ Game stabilization period complete (30s)
✓ Previous PRX unloaded
✓ SPRX injection SUCCESSFUL!
✓ IPC file found! Parsing addresses...
✓ Preamble installed on cellUsbdInit
✓ Preamble installed on cellUsbdOpenPipe
✓ Preamble installed on cellUsbdTransfer
✓ Preamble installed on cellUsbdClosePipe
✓ All preambles installed!
```

---

## 9. Log Reference

### Boot Log (`/dev_hdd0/plugins/ldtoypad_boot.log`)
Plain-text with key milestones:
```
=== ldtoypad Full Integration: module_start ===
Game process confirmed — creating worker thread...
OK: debug_init()
OK: network_init(28472)
OK: network_wait_ready()
OK: network_set_server(192.168.0.17:28472)
OK: usb_hook_init() -- USB hooks installed for game
OK: toypad_state_init()
=== Entering main loop ===
```

### Debug Log (`/dev_hdd0/plugins/ldtoypad_debug.log`)
Verbose with `[LDTP]` prefix tags:
```
[LDTP] [MAIN] Worker thread entering init chain
[LDTP] [NET] network_init(port=28472) ENTRY
[LDTP] [NET] socket()+bind() succeeded on attempt 1
[LDTP] [USB] Scanning game memory for cellUsbd NIDs...
[LDTP_ERROR] [USB] (NID scan failure message)
```

### IPC File (`/dev_hdd0/tmp/ld_hooks_ready.txt`)
Expected format (when working):
```
cellUsbdInit|0xXXXXXX|0xYYYYYY
cellUsbdOpenPipe|0xXXXXXX|0xYYYYYY
cellUsbdTransfer|0xXXXXXX|0xYYYYYY
cellUsbdClosePipe|0xXXXXXX|0xYYYYYY
```
Where `0xXXXXXX` = target function address, `0xYYYYYY` = trampoline address.
Heartbeat address at offset 128 in trampoline page.

---

## 10. Remaining Risks

| Risk | Mitigation |
|------|-----------|
| **NID scanner cannot find game functions** | Move to PC-side ELF parsing (proposed fix) |
| **sendto fails (-2147417535) when bridge not running** | Silently ignore ENETDOWN errors in loop |
| **recvfrom blocks if O_NONBLOCK not applied** | Verify setsockopt(SO_NBIO) actually works |
| **sys_memory_allocate returns non-executable page** | Expert confirmed CellOS returns R-W-X by default |
| **toc_trampoline.s OPD linkage broken** | Both .s and _c.c compile and link without errors |
| **Injection timing affects game stability** | 30s stabilization wait + MODULE UNLOAD before LOAD |

---

## 11. Quick Reference URLs (PC → PS3 webMAN)

| Operation | URL |
|-----------|-----|
| Game PID | `GET /ps3mapi.ps3?PROCESS%20GETCURRENTPID` |
| Load SPRX | `GET /ps3mapi.ps3?MODULE%20LOAD%200xPID%20%2Fpath` |
| Unload SPRX | `GET /ps3mapi.ps3?MODULE%20UNLOAD%200xPID%20%2Fpath` |
| Memory Write | `GET /ps3mapi.ps3?MEMORY%20SET%200xPID%200xADDR%20HEXDATA` |
| IPC File | `GET /dev_hdd0/tmp/ld_hooks_ready.txt` |
| Boot Log | `GET /dev_hdd0/plugins/ldtoypad_boot.log` |
| Debug Log | `GET /dev_hdd0/plugins/ldtoypad_debug.log` |
| FTP Root | `ftp://mike:mike@192.168.0.47/` |

---

*End of complete handoff. All known state and issues documented above.*
