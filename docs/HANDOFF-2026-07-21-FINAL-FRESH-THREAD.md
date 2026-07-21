# LD-ToyPad Bridge — Agent Handoff 2026-07-21
## Ready for Fresh Agent Thread — All Expert Guidance Incorporated

---

## 1. Project State Summary

**Two blocking issues exist, both now fully diagnosed with expert-closed answers:**

| Issue | Status | Expert Verdict |
|-------|--------|---------------|
| **A: PID parser mismatch** | ✅ **Fixed** — needs live test | `"response"` key is correct for all webMAN MOD 1.47.48c+ JSON endpoints |
| **B: SPRX network_init() fails** | **Source fixed, needs rebuild+deploy+test** | Previous orphaned socket; MODULE UNLOAD is synchronous; 2s delay + 120 retries + explicit unload before load should resolve |

---

## 2. Expert Answers (All Questions Closed)

### Q-EXPERT-A1: JSON key consistency
> **Answer:** `"response"` key is CONSISTENT across ALL `/ps3mapi.ps3?` endpoints. `MODULE LOAD`, `MODULE UNLOAD`, `MEMORY SET` all return `{"response": "..."}`. The parser fix checking `json.response` is universally correct.

### Q-EXPERT-B1: MODULE UNLOAD timing
> **Answer:** MODULE UNLOAD is **SYNCHRONOUS**. The LV2 kernel halts the calling thread and invokes `module_stop()` before sending the HTTP response. The HTTP response is NOT sent until `module_stop()` returns.
>
> **If the port is still held after unload, check:**
> 1. Is `O_NONBLOCK` actually applied to the socket? If `recvfrom()` blocks, `g_shutdown = 1` is never evaluated, the thread never exits, and `sys_ppu_thread_join()` hangs.
> 2. Is `sys_ppu_thread_join()` actually being called in `module_stop()`?

### Q-EXPERT-B2: PID format
> **Answer:** `0x0000000001010200` is the **exact intended format**. PS3 LV2 kernel PIDs are `uint64_t`. webMAN MOD formats as 16-char padded hex. `parseInt(hexStr, 16)` correctly strips leading zeros.

### Q-EXPERT-B3: Intermittent timeouts
> **Answer:** **Normal.** webMAN MOD runs on a low-priority thread. During heavy I/O (game loading, trophy sync), the HTTP daemon stalls. Catch errors silently and continue polling.

---

## 3. What Has Been Done (DO NOT REDO)

### inject-sprx.js — All 4 URL migrations + PID parser fix
All changes already saved to disk. Key changes:

| Function | OLD (broken) | NEW (working) |
|---|---|---|
| Game detection | `GET /ps3mapi_process` (400) | `GET /ps3mapi.ps3?PROCESS%20GETCURRENTPID` |
| SPRX inject | `GET /ps3mapi_process?pid=&load_prx=` (400) | `GET /ps3mapi.ps3?MODULE%20LOAD%200xPID%20/path` |
| SPRX unload | Same (400) | `GET /ps3mapi.ps3?MODULE%20UNLOAD%200xPID%20/path` |
| IPC polling | `GET /cpursx.ps3?/read_process?path=...` | `GET /dev_hdd0/tmp/ld_hooks_ready.txt` |
| Preamble writes | `GET /cpursx.ps3?/write_process...` | `GET /ps3mapi.ps3?MEMORY%20SET%200xPID%200xADDR%20HEX` |
| PID parser | Looked for `json.result` | Looks for `json.response`, falls back to `json.result` |

### network.c — Fixes applied
- ✅ ENTRY debug log at function start
- ✅ 2-second stabilization delay before retry loop
- ✅ 30→120 retries (12 seconds total at 100ms)
- ✅ Fixed `last_bind_err` (stores `bind_ret`, not `g_net.socket_fd`)
- ✅ Explicit `IPPROTO_UDP` in socket() call
- ✅ Comment about NO SO_REUSEADDR (expert confirmed: UDP has no TIME_WAIT)

### main.c — Fixes applied
- ✅ 10ms→50ms sleep (20 Hz) to prevent sceNpTrophy starvation
- ✅ Memory-mapped heartbeat in worker loop (at offset 128 in trampoline page)
- ✅ `module_stop()` closes socket + joins thread + finalizes sys_net

### usb_hooks.c — Fixes applied
- ✅ Atomic IPC file writes (`.tmp` → `cellFsRename`)
- ✅ sys_memory_allocate for executable trampoline pages (R-W-X)
- ✅ Heartbeat pointer at offset 128 in trampoline page
- ✅ NID scanner with 20-second PLT retry loop

---

## 4. What Needs to Be Done (Step-by-Step for New Agent)

### Step 1: Rebuild the SPRX
```bash
cd /mnt/c/Users/Admin/source/repos/dimensions\ plugin/sprx-plugin
make clean && make
```

### Step 2: Deploy the SPRX to PS3
```powershell
.\ftp-deploy.ps1
```

### Step 3: Start the bridge server
```bash
node ld-toypad-server/server.js
```

### Step 4: Boot LEGO Dimensions on PS3
Wait until "Connect Toy Pad" screen is visible.

### Step 5: Run the injector
```bash
node ld-toypad-server/scripts/inject-sprx.js --verbose --wait 30
```

### Step 6: Check boot log for network_init success
```powershell
.\pull-papertrail.ps1
# Check papertrail_ldtoypad_boot.log for:
# OK: network_init(28472)
# OK: network_wait_ready()
# OK: usb_hook_init() -- USB hooks installed for game
```

### Step 7: Validate IPC file
Check `ld_hooks_ready.txt` appears with all 4 target+wrapper addresses.

### Step 8: Validate preamble installation
Check injector outputs "✓ Preamble installed on cellUsbdInit" etc.

---

## 5. Key Source Files

| File | Purpose | Status |
|------|---------|--------|
| `ld-toypad-server/scripts/inject-sprx.js` | Game detection, SPRX injection, preamble install | **READY** |
| `sprx-plugin/main.c` | Worker thread, init chain, module_start/stop | **READY** |
| `sprx-plugin/network.c` | UDP socket, retry loop, keepalive | **READY** |
| `sprx-plugin/network.h` | Network state struct, packet types, extern g_net | **READY** |
| `sprx-plugin/usb_hooks.c` | NID scanner, trampoline allocation, IPC file, my_cellUsbd* | **READY** |
| `sprx-plugin/usb_hooks.h` | usb_hook_state_t struct, function declarations | **READY** |
| `sprx-plugin/hook.h` | PowerPC opcode builders (no more hook_install/remove) | **READY** |
| `sprx-plugin/toc_trampoline.s` | Assembly TOC wrappers (saves/restores r2) | **NEEDS VERIFICATION** |
| `sprx-plugin/toc_trampoline_c.c` | OPD definitions for call_original_* stubs | **NEEDS VERIFICATION** |
| `sprx-plugin/Makefile` | Build targets, must include toc_trampoline.s | **NEEDS VERIFICATION** |
| `sprx-plugin/debug.h` | DEBUG_PRINT macros | **READY** |
| `sprx-plugin/debug.c` | Debug logging to HDD | **READY** |
| `sprx-plugin/compat.c` | SDK compatibility shims (IPPROTO_UDP) | **READY** |

---

## 6. Two-Phase Hook Architecture (How It Works)

```
PHASE 1 (SPRX-side, runs in game process):
  1. module_start → worker thread created
  2. network_init() → socket + bind (+ retry loop)
  3. usb_hook_init():
     a. NID scan finds cellUsbd* function addresses in game .text
     b. sys_memory_allocate allocates 64KB R-W-X page
     c. Allocate 4 × 32-byte trampoline blocks in the page
     d. Copy 4 original instructions from each target into trampoline
     e. Build branch-back (lis/ori/mtctr/bctr) in trampoline
     f. Heartbeat pointer at offset 128
     g. Write IPC file: /dev_hdd0/tmp/ld_hooks_ready.txt
  4. Main loop: recv, heartbeat++, probe, keepalive, sleep 50ms

PHASE 2 (Node.js-side, inject-sprx.js):
  1. Poll GET /ps3mapi.ps3?PROCESS%20GETCURRENTPID → get game PID
  2. Wait for stabilization period
  3. MODULE UNLOAD old PRX (synchronous, waits for module_stop)
  4. MODULE LOAD new SPRX
  5. Poll GET /dev_hdd0/tmp/ld_hooks_ready.txt → get target+wrapper addrs
  6. For each of 4 functions:
     a. Build 4-instruction preamble (lis/ori/mtctr/bctr)
     b. MEMORY SET via PS3MAPI (Ring 0 bypasses R-X protection)
  7. Game now routes cellUsbd calls through hooks
```

---

## 7. Remaining Risks

| Risk | Mitigation |
|------|-----------|
| **network_init() still fails** even with 120 retries | Check boot log; may need to verify `SO_NBIO` is actually applied (expert hint: if `recvfrom` is blocking, `module_stop` hangs) |
| **NID scan finds no targets** | NID values might be wrong for BLUS31548/BLES02206 variant; need dynamic resolution |
| **Trampoline page not executable** | Expert confirmed `sys_memory_allocate` returns R-W-X pages by default on CellOS |
| **`toc_trampoline.s` not integrated in Makefile** | Must verify `.s` file is compiled and linked into SPRX |
| **`toc_trampoline_c.c` OPD linkage broken** | Must verify extern function pointers resolve correctly |

---

## 8. File: inject-sprx.js (Current State)

**Location:** `ld-toypad-server/scripts/inject-sprx.js`

The file has been fully refactored. Key behaviors for the new agent to be aware of:

- Parses `json.response` (not `json.result`)
- Handles 64-bit PID format with leading zeros
- Unloads previous PRX before loading new one (with 500ms delay)
- Retries injection once on failure
- Builds preambles as 16-byte buffers (lis/ori/mtctr/bctr)
- Writes via `MEMORY SET` endpoint

Expected successful output:
```
✓ webMAN detected
✓ Game detected! PID=0x1010200 (16843032)
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

**If injection fails:** The SPRX may still be crashing at `network_init()`. Check `/dev_hdd0/plugins/ldtoypad_boot.log` for `FAIL: network_init()`.

---

## 9. Files That Need Verification

The following files were part of the refactoring but their correct integration has NOT been tested:

1. **`sprx-plugin/toc_trampoline.s`** — Does the Makefile compile this? Is the assembly syntax correct for CellOS GAS (GNU Assembler)?

2. **`sprx-plugin/toc_trampoline_c.c`** — Do the extern function pointers correctly link to the assembly labels? Are OPD entries correctly formed?

3. **`sprx-plugin/Makefile`** — Verify these build targets exist:
   - `toc_trampoline.o` (from `.s`)
   - `toc_trampoline_c.o` (from `.c`)
   - Both linked into the final `.sprx`

4. **`sprx-plugin/compat.c`** — Verify `IPPROTO_UDP` definition exists.

---

## 10. How to Validate Full Pipeline

1. **SSH into WSL or use Git Bash** for make commands
2. **Rebuild**: `cd sprx-plugin && make clean && make`
3. **Deploy**: `./ftp-deploy.ps1` (or drag to PS3 via FTP)
4. **Start server**: `node ld-toypad-server/server.js`
5. **On PS3**: Boot LEGO Dimensions, get to "Connect Toy Pad"
6. **Inject**: `node ld-toypad-server/scripts/inject-sprx.js --verbose`
7. **If fails**: Pull papertrail via `./pull-papertrail.ps1` and check boot log
8. **If succeeds**: The server should show RX packets from the PS3

---

*End of handoff. Build, deploy, test.*
