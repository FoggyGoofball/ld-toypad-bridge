# HANDOFF — 2026-07-23 19:20 CST

## Status: BUILD SUCCESSFUL + SPRX DEPLOYED TO PS3

## Summary
- **Build**: ldtoypad.sprx (19776 bytes) successfully compiled, linked, signed, and deployed
- **SPRX deployed via FTP** to `/dev_hdd0/plugins/ldtoypad.sprx`
- **boot_plugins.txt** normalized to LF-only (Cobra-safe)
- **Next step**: Reboot PS3, launch LEGO Dimensions, verify NID scan finds the 4 cellUsbd functions

---

## Changes Made (this session)

### Major Fix: NID Scan Region Expansion
**File**: `sprx-plugin/usb_hooks.c` — `g_scan_regions[]`
- **Problem**: The NID scanner was searching a single region `0x100000-0xB00000` (10MB). The debug log showed NIDs not found in this range.
- **Fix**: Expanded to **5 regions covering 0x00100000-0x05000000 (80MB)**, matching the PC-side `scan-game-nids.js` scanner:
  - Region 1: `0x00100000` - `0x00900000` (game .text region 1)
  - Region 2: `0x00900000` - `0x01000000` (gap region)
  - Region 3: `0x01000000` - `0x02000000` (game .text region 2)
  - Region 4: `0x02000000` - `0x03000000` (game .text region 3)
  - Region 5: `0x03000000` - `0x05000000` (extended high region)
- The deployed binary on the PS3 was the OLD code (single region). The newly deployed build has all 5 regions.

### Debug Log Improvements
- `sprx-plugin/debug.h` — Re-enabled `DEBUG_PRINT` macro in `usb_hooks.c` context so NID scan findings are visible in papertrail
- `sprx-plugin/debug.c` / `sprx-plugin/main.c` — Increased verbosity for init chain diagnostics

### Modified Files (git status)
```
modified:   ld-toypad-server/scripts/inject-sprx.js
modified:   pull-papertrail.ps1
modified:   sprx-plugin/debug.c
modified:   sprx-plugin/debug.h
modified:   sprx-plugin/main.c
modified:   sprx-plugin/usb_hooks.c
```

---

## Architecture (current state)

### Dynamic Trampoline System
- **`trampoline_gen.c`** + **`trampoline_gen.h`** — Generate 64-byte (16-instruction) PowerPC trampolines at runtime in R-W-X memory
- **`usb_hooks.c`** — Orchestrates hook lifecycle:
  1. `find_cellusbd_functions_via_opd()` — Validate cellUsbd imports (SOFT fail allowed)
  2. `install_hooks()` — Allocate 64KB R-W-X page via `sys_memory_allocate`, call `create_hook_trampoline()` for each of 4 hooks
  3. `get_game_plt_stub()` — Scan game memory for NID triplets → read GOT → get PLT stub addresses
  4. `write_ipc_file()` — Write TRAMP_* and TARGET_* addresses to `/dev_hdd0/tmp/ld_hooks_ready.txt`

### Key Files
| File | Purpose |
|------|---------|
| `sprx-plugin/main.c` | Entry point, init chain, worker loop |
| `sprx-plugin/usb_hooks.c` | Hook init, NID scanner, IPC writer |
| `sprx-plugin/trampoline_gen.c` | Dynamic PowerPC trampoline generator |
| `sprx-plugin/network.c` | UDP networking (beacon, data, polling) |
| `sprx-plugin/toypad_state.c` | Toy Pad state machine |
| `sprx-plugin/debug.c` | Paper-trail logging |
| `ld-toypad-server/server.js` | Node.js orchestrator (bridge) |
| `ld-toypad-server/scripts/inject-sprx.js` | PS3MAPI preamble writer |

### Deleted/Replaced Files
- `toc_trampoline.s` — Removed (static assembly no longer needed)
- `toc_trampoline_c.c` — Removed (replaced by trampoline_gen.c)
- `hook.h` — Kept but deprecated (references in usb_hooks.c removed)

---

## How Hooks Work (Step-by-Step)

1. **PS3 boots**, Cobra loads `ldtoypad.sprx` from `boot_plugins.txt`
2. **SPRX init**: Network layer starts, UDP beacon to PC
3. **SPRX USB init** (`usb_hook_init()`):
   - OPD extraction of cellUsbd imports (validation only)
   - `sys_memory_allocate(64KB)` → R-W-X page at ~0x10320000
   - 4 trampolines generated (64 bytes each) at offsets 0/64/128/192
   - NID scanner searches game memory for 4 cellUsbd NID triplets
   - Reads GOT slot → gets PLT stub address (TARGET_*)
   - Writes IPC file: TRAMP_* = trampoline addresses, TARGET_* = PLT stub addresses
4. **Node.js injector** (`inject-sprx.js`):
   - Polls HTTP GET for `ld_hooks_ready.txt`
   - Reads TARGET_* addresses
   - **Writes 4-instruction preamble** via PS3MAPI `/write_process`:
     ```
     lis   r12, trampoline_addr_hi     # 0x3D80xxxx
     ori   r12, r12, trampoline_addr_lo  # 0x618Cxxxx
     mtctr r12                          # 0x7D8903A6
     bctr                               # 0x4E800420
     ```
   - This overwrites the game's PLT stub with a branch to our trampoline
5. **Game calls cellUsbdOpenPipe()**:
   - Game's PLT stub → preamble → trampoline
   - Trampoline saves game's TOC, loads SPRX's TOC, calls C hook
   - C hook checks if ToyPad → handle ourselves
   - Non-ToyPad → call real `cellUsbdOpenPipe()` directly (SPRX's own import)

---

## Verification Steps (for next agent)

### 1. Reboot PS3 and launch LEGO Dimensions
```powershell
# From dev machine:
.\pull-papertrail.ps1
# Read papertrail_ldtoypad_debug.log
```

### 2. Look for these key log lines

**Expected — NID scan succeeding:**
```
[LDTP] [USB] NID scan: game .text region 1: NID=0x7F5F00D3 at 0x00XXXXXX GOT_ptr=0x00XXXXXX -> *GOT=0x00XXXXXX (ok, unresolved)
[LDTP] [USB] NID scan: game .text region 2: NID=0x1AB6D80B at 0x00XXXXXX GOT_ptr=0x00XXXXXX -> *GOT=0x00XXXXXX (ok, unresolved)
... (4 total)
[LDTP] [USB] TARGET_INIT=0x00XXXXXX -> TRAMP_INIT=0x10320000
[LDTP] [USB] TARGET_OPENPIPE=0x00XXXXXX -> TRAMP_OPENPIPE=0x10320040
[LDTP] [USB] TARGET_TRANSFER=0x00XXXXXX -> TRAMP_TRANSFER=0x10320080
[LDTP] [USB] TARGET_CLOSEPIPE=0x00XXXXXX -> TRAMP_CLOSEPIPE=0x103200C0
[LDTP] [USB] All 4 hooks installed, awaiting Node.js preamble
```

**If NIDs still not found — implement Option B (PLT pattern scanner):**
```
[LDTP_ERROR] [USB] NID scan: NID=0x7F5F00D3 not found in any region (0x100000-0x5000000)
```
If this happens, the GOT was resolved before our scan. Implement the PLT-pattern scanner described in the `get_game_plt_stub()` fallback section.

### 3. Check injector log
```
[INJECTOR] Read IPC: TRAMP_INIT=0x10320000, TRAMP_OPENPIPE=0x10320040, ...
[INJECTOR] Writing preamble to TARGET_INIT=0x00XXXXXX
[INJECTOR] Hook cellUsbdInit installed at 0x00XXXXXX -> 0x10320000
[INJECTOR] All 4 hooks installed via PS3MAPI /write_process
```

### 4. Verify inject-sprx.js can connect
The injector needs `PS3MAPI` running on the PS3. The bridge server expects the `inject-sprx.js` script to be called after the IPC file appears.

---

## Known Issues / Future Work

### 1. Option B — PLT Pattern Scanner (High Priority if NIDs fail)
The current `get_game_plt_stub()` reads the GOT slot value (`*got_ptr`). If the game has already called cellUsbdInit, the GOT is resolved and `*got_ptr` points to libusbd.sprx, not the PLT stub. Implement a pattern scanner that finds PLT stubs directly:
```
lis r12, offset_hi      0x3D80xxxx
lwz r12, offset_lo(r12) 0x818Cxxxx
mtctr r12               0x7D8903A6
bctr                    0x4E800420
```
The `g_scan_regions[]` FALLBACK section in `usb_hooks.c` has the design notes.

### 2. network.c getsockname Fails on PS3
The log shows `Could not determine self IP after 50 polls`. The server hardcodes `192.168.0.17:28472` as fallback. This works but is fragile. UDP works despite the failure.

### 3. OPD Extraction Warnings Are Expected
```
[LDTP_ERROR] [USB] OPD: cellUsbdInit code_addr=0x7B0748 out of range - likely import stub
```
This is the OPD of our **imported** cellUsbd symbols, which point to import stub code, not real OPDs. This is safe — `create_hook_trampoline()` uses the OPDs of **our own C hooks** (my_cellUsbdInit, etc.), which are always valid.

### 4. Firewall/Network Issues
If `sendto failed: -2147417535` appears, the PC bridge server isn't reachable. Check:
- Server running: `node server.js`
- Firewall allows UDP port 28472 inbound/outbound
- PS3 can ping PC

---

## Build & Deploy Commands

```powershell
# Build SPRX
powershell -ExecutionPolicy Bypass -File "c:\Users\Admin\source\repos\dimensions plugin\sprx-plugin\build-all.ps1"

# Deploy to PS3
powershell -ExecutionPolicy Bypass -File "c:\Users\Admin\source\repos\dimensions plugin\ftp-deploy.ps1"

# Get debug logs
powershell -ExecutionPolicy Bypass -File "c:\Users\Admin\source\repos\dimensions plugin\pull-papertrail.ps1"
```

---

*Handoff by Cline, 2026-07-23 19:20 CST*
*SPRX built, signed, deployed. Next: reboot PS3, launch game, check NID scan.*
