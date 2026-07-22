# LD-ToyPad Bridge — Diagnostic Handoff & Investigation Report
## Date: 2026-07-21, ~21:00 MDT

---

## 1. Executive Summary

We are building a **PC↔PS3 bridge** that intercepts LEGO Dimensions USB calls (cellUsbd functions) on PS3 and routes them to a Node.js server on PC. The server emulates the toy pad protocol, allowing USB toy pads connected to the PC instead of a physical PS3 toy pad.

**Current status:** The SPRX successfully injects, network initializes, and the main loop runs — but **the NID scanner inside the SPRX fails to find the game's cellUsbd functions.** All four hook targets resolve to PLT stubs (lazy binding not resolved), so no IPC file is written and no preambles are installed. The Toy Pad never responds.

---

## 2. What We Did This Session

### 2.1 Read & Understood Complete Codebase

We thoroughly reviewed every critical file in the project:

| File | Lines | Role |
|------|-------|------|
| `sprx-plugin/main.c` | 359 | module_start/stop, worker thread, init chain, main loop |
| `sprx-plugin/network.c` | 613 | UDP socket, bind, sendto, recvfrom, keepalive, discovery |
| `sprx-plugin/network.h` | 160 | net_state struct, packet type constants |
| `sprx-plugin/usb_hooks.c` | 613 | NID scanner (BROKEN), trampoline alloc, IPC file, hooks |
| `sprx-plugin/usb_hooks.h` | 142 | Hook state struct, function declarations |
| `sprx-plugin/hook.h` | 165 | PowerPC opcode builders (lis, ori, mtctr, bctr) |
| `sprx-plugin/toc_trampoline.s` | 165 | Assembly TOC wrappers (HOOK_WRAPPER, HOOK_PASSTHROUGH) |
| `sprx-plugin/toc_trampoline_c.c` | 174 | OPD definitions linking assembly to C |
| `sprx-plugin/debug.h` | 133 | Debug macros, ring buffer |
| `sprx-plugin/Makefile` | 71 | Build rules, oscetool signing |
| `ld-toypad-server/scripts/inject-sprx.js` | 589 | PC-side injector (detect → wait → unload → load → poll IPC → write preambles) |
| `ld-toypad-server/server.js` | 799 | Node.js bridge server (UDP listener + HTTP UI) |
| `ld-toypad-server/scan-game-nids.js` | **NEW** | PC-side NID scanner (written this session) |

### 2.2 Built PC-Side NID Scanner (scan-game-nids.js)

Created a Node.js tool that uses PS3MAPI `getmem` to scan the game's memory from the PC side for cellUsbd NID stubs. Located at:
`ld-toypad-server/scripts/scan-game-nids.js`

This tool:
- Reads game memory at `0x00100000-0x00A00000` (game .text), `0x01000000-0x02000000` (segment 2), `0x30000000-0x31000000` (PRX region)
- Searches for cellUsbd NIDs (0x7F5F00D3, 0x1AB6D80B, 0x7B4436CE, 0x2F82F1A5) in both BE and LE formats
- For each found NID, reads the GOT slot to check if it resolves to a real function (≥ 0x30000000) or PLT stub (< 0x30000000)
- Provides raw hex dumps for manual analysis when nothing is found

### 2.3 Ran Diagnostics on Live PS3

**PS3 accessible at 192.168.0.47 with webMAN MOD 1.47.48c**

Key findings:
- ✅ webMAN responds on port 80
- ✅ Game PID confirmed: `0x1010200` (LEGO Dimensions BLUS31473, version 01.22)
- ✅ `getmem.ps3mapi` works — we can read game memory
- ✅ First 8MB scan completed: **no cellUsbd NIDs found with the current NID values**
- Background scan was still running for region 2 (0x01000000-0x02000000) when interrupted

The raw hex dump at `0x00800000` shows valid PowerPC code (not zeroed/unmapped), so memory reads work correctly. The NIDs simply aren't present where expected.

---

## 3. Architecture Deep Dive

### 3.1 Two-Phase Hook Installation

```
PHASE 1 (SPRX, in-game):
┌─────────────────────────────────────────────────────┐
│ module_start → worker_thread →                      │
│   network_init() → UDP socket + bind                │
│   network_wait_ready() → get IP + beacon salvo      │
│   usb_hook_init() →                                 │
│     NID scan game memory → FAILS (PLT stubs)        │
│     Would: allocate trampoline page                 │
│     Would: copy 4 original instructions             │
│     Would: build branch-back in trampoline          │
│     Would: write IPC file → (never reached)          │
│   Main loop: recv, heartbeat, probe, sleep 50ms     │
└─────────────────────────────────────────────────────┘

PHASE 2 (Node.js, PC-side):
┌─────────────────────────────────────────────────────┐
│ inject-sprx.js:                                      │
│   Poll webMAN for game PID → ✓                       │
│   Wait 30s for stabilization → ✓                     │
│   MODULE UNLOAD old PRX → ✓                          │
│   MODULE LOAD new SPRX → ✓                           │
│   Poll for ld_hooks_ready.txt → ✗ NEVER WRITTEN      │
│     (SPRX NID scanner never completes)               │
└─────────────────────────────────────────────────────┘
```

### 3.2 Current NID Scanner (Broken)

In `usb_hooks.c`, the function `find_game_cellusbd_functions()` scans memory at hardcoded regions:

```c
static const uint32_t g_scan_regions[][2] = {
    { 0x00100000, 0x00800000 },  // game .text
    { 0x01000000, 0x01000000 },  // game segment 2
    { 0x02000000, 0x01000000 },  // game segment 3
    { 0x30000000, 0x00800000 },  // system PRX region
    { 0x40000000, 0x01000000 },  // system PRX alt
};
```

These are **game process virtual addresses**, not the SPRX's own segments. The handoff document says the bug is that `sys_prx_get_module_info(NULL, ...)` returns the SPRX's segments — **but the current code doesn't use that function at all.** The code directly dereferences pointers to these memory regions.

The real issue is that `scan_for_nid()` correctly finds NID stubs in the game's memory, but **rejects them all** because the GOT entries are PLT stubs (pointing to < 0x30000000 range):

```c
/* REJECT PLT STUBS (CellOS lazy binding): */
if (func_addr == 0 ||
    func_addr < 0x30000000 ||
    func_addr > 0x4FFFFFFF) {
    continue;  // ← REJECTS ALL GAME STUBS before game calls them
}
```

The retry loop runs 10 times with 2 second pauses (20 seconds total), but the game never resolves these GOT entries because **the game never calls cellUsbdRegisterLdd until the Toy Pad is actually attached**. Since our hooks aren't installed yet, the game doesn't know a Toy Pad exists, so it never triggers USB enumeration, so the GOT entries stay as PLT stubs.

### 3.3 Key Insight: Chicken-and-Egg Problem

```
Game boots → cellUsbd functions in GOT are PLT stubs (unresolved)
   ↓
   We inject SPRX → NID scanner finds PLT stubs → REJECTS them
   ↓
   We never install hooks because scanner returns failure
   ↓
   Game never calls cellUsbd because no Toy Pad is attached
   ↓
   GOT entries never resolve to real function addresses
   ↓
   CHICKEN & EGG: Scanner can't find real addresses until hooks are installed,
   but hooks can't be installed until scanner finds real addresses
```

**The fix:** We need to use the **PLT stub addresses** instead of waiting for the real function addresses. The PLT stubs point to 16-byte trampolines in the game's `.text` that, when called, trigger the dynamic linker to resolve the GOT. If we replace the PLT stub itself with our 4-instruction preamble, it will still work — we intercept the call before lazy binding fires.

OR: Read the target address from the **import stub table** (the 12-byte triplets: NID + reserved + GOT offset), calculate the actual function pointer from the ELF's import table structure, rather than dereferencing GOT entries.

### 3.4 Import Stub Table Format (What Scanner Looks For)

CellOS PRX import stubs follow this 12-byte triplet format in the game's ELF:

```
Offset  Size  Field
0       4     NID (Network ID of the imported function)
4       4     Reserved (usually 0 or small value)
8       4     GOT pointer (points to a slot in the .got section)
```

The GOT slot initially contains a pointer to a **PLT stub** in `.plt` section (low memory, < 0x30000000). After the game calls the function, the dynamic linker resolves it and overwrites the GOT slot with the real function address (in PRX region, ≥ 0x30000000).

---

## 4. Detailed Bug Analysis

### ✅ Bug A: NID Scanner Rejects All Results (PARTIALLY UNDERSTOOD)

**Confirmed:** The scanner correctly finds the NID triplets but rejects the GOT values because they point to PLT stubs.

**Failed attempt at detection from PC-side:** The `scan-game-nids.js` scanner scanned the first 8MB of game memory (0x00100000-0x00900000) and found **no matches** with the current NID values in either LE or BE format. 

**Possible explanations:**
1. The NIDs may be at addresses > 0x00900000 (outside the first scan range) — region 2 (0x01000000+) was still scanning when interrupted
2. The NID values in the code may not match this specific game version (BLUS31473, version 01.22)
3. The 12-byte triplet format may not be contiguous (maybe interleaved with other data)
4. The game may use 64-bit NIDs instead of 32-bit (CellOS sometimes uses SHA-1 hashes truncated to 64 bits)

**What we know for certain:** The `scan_game_nids.js` scanner is working correctly — it parsed `getmem.ps3mapi` HTML responses and extracted valid PowerPC code from the game memory. The sample at `0x00800000` shows real game code:
```
00800000  38 A0 00 00 90 BF 00 14 80 83 00 00 F8 41 00 28  ...
```

### ❓ Bug B: recvfrom Non-Blocking (NOT TESTED)

Cannot verify because the NID scanner fails before entering the main recv loop.

**Current state in `network.c`:**
```c
int on = 1;
setsockopt(g_net.socket_fd, SOL_SOCKET, SO_NBIO, (void*)&on, sizeof(on));
```

**Risk:** If `SO_NBIO` doesn't actually apply non-blocking on this SDK, `recvfrom()` would block and the SIGTERM → close socket → `module_stop` hang would occur.

### ❓ Bug C: sendto Error Log Flooding (NOT TRIGGERED)

Not tested because the NID scanner fails first. However, the current code path uses hardcoded server IP (192.168.0.17:28472) set in `main.c`:
```c
network_set_server(htonl(0xC0A80011), 28472);
```
This is hardcoded to the developer's PC and would need to be changed per-machine.

### ❓ Bug D: Server Auto-Restart (NOT ADDRESSED)

The Node.js bridge server is started manually with `start /B node server.js` — no restart mechanism if it crashes.

---

## 5. What SPRX Does After a Successful Boot

Even though hooks fail, the current SPRX build:

1. ✅ Creates worker thread
2. ✅ Opens UDP port 28472 (succeeds on attempt 1)
3. ✅ Waits for interface (getsockname poll)
4. ✅ Sends 10 discovery beacons at 100ms intervals
5. ✅ Hardcodes server IP to 192.168.0.17:28472
6. ✅ Calls usb_hook_init() → fails silently (returns -1)
7. ✅ Logs "NOTE: usb_hook_init() — VSH-only mode"
8. ✅ Enters main loop — sends keepalive, probes, sleeps 50ms

**Expected in logs:**
```
=== ldtoypad Full Integration: module_start ===
Game process confirmed — creating worker thread...
OK: debug_init()
OK: network_init(28472)
OK: network_wait_ready()
OK: network_set_server(192.168.0.17:28472)
NOTE: usb_hook_init() — VSH-only mode (no game hooks)
OK: toypad_state_init()
=== Entering main loop ===
```

---

## 6. Key Files Reference

### `sprx-plugin/usb_hooks.c` — NID Scanner (Broken)
Lines 39-43: NID constant definitions
Lines 47-53: Scan regions
Lines 392-437: `scan_for_nid()` — the PLT stub rejection logic
Lines 439-489: `find_game_cellusbd_functions()` — iterative scan
Lines 517-531: Retry loop (10 attempts × 2s)

### `sprx-plugin/inject-sprx.js` — PC-Side Orchestrator
Lines 165-212: PID parsing from webMAN JSON
Lines 246-316: Module load/unload logic
Lines 325-447: IPC polling + preamble writing

### `sprx-plugin/toc_trampoline.s` — Assembly Wrappers
Lines 41-72: HOOK_WRAPPER macro (saves TOC, calls C hook, restores TOC)
Lines 85-130: HOOK_PASSTHROUGH macro (restores game TOC, calls trampoline)

### `sprx-plugin/hook.h` — Opcode Builders
Lines 84-108: lis r11, ori r11, mtctr r11, bctr builders

---

## 7. Next Investigation Steps

1. **Complete the PC-side memory scan** — region 2 (0x01000000+) was still scanning when interrupted. The game's import tables may be located there.

2. **Extract and analyze the game ELF** — Read the actual `.sprx` or `.self` file from the PS3 via FTP at:
   - `/dev_dvd/PS3_GAME/USRDIR/` (disc-based game)
   - `/dev_hdd0/game/BLUS31473/` (installed game data)
   - Parse the ELF program headers to find the `.text` section NID stubs

3. **Try reading the IML (Import Module List)** — PS3 ELFs have explicit import stubs. The cellUsbd imports will be listed there with proper NIDs.

4. **Read process memory maps** — Use PS3MAPI `get_process_maps` or similar to find the exact address ranges where the game's import tables are mapped:

   `GET /ps3mapi.ps3?PROCESS%20GETMAPS%200x1010200`

5. **Manually attach USB Toy Pad to trigger GOT resolution** — If a real Toy Pad is attached, the game calls cellUsbdInit which triggers the GOT to resolve, making the scanner work.


