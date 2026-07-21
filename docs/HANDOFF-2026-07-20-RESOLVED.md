# LD-ToyPad Bridge: Resolved Defect Handoff
## 2026-07-20 — Build 16,592 bytes SPRX (Sony SDK 3.40)

---

## Abstract

This document supersedes `HANDOFF-2026-07-20-CRITICAL-BUGS.md`. All five critical
userland hooking defects have been resolved. The SPRX builds, signs, and deploys
successfully. This report details the resolutions and documents the current
architecture for incoming agents.

---

## 1. Build Verification

| Artifact         | Path                                          | Size     |
|------------------|-----------------------------------------------|----------|
| PRX (unstripped) | `sprx-plugin/build/ldtoypad.prx`              | 99,556 B |
| SPRX (signed)    | `sprx-plugin/build/ldtoypad.sprx`             | 16,592 B |
| SPRX (plugin/)   | `sprx-plugin/plugin/ldtoypad.sprx`            | 16,592 B |
| SPRX (deployed)  | `/dev_hdd0/plugins/ldtoypad.sprx` (PS3 FTP)   | 16,592 B |

Build command:
```bash
cd '/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin'
make clean && make
```

Signing (when Makefile path has spaces):
```bash
cp build/ldtoypad.prx /tmp/ldtoypad.prx
cd ~/oscetool-src
./oscetool -v -0 SELF -1 TRUE -2 000A \
  -3 1010000001000003 -4 01000002 \
  -A 0001000000000000 -5 APP \
  -8 4000000000000000000000000000000000000000000000000000000000000002 \
  -6 0004009300000000 -e /tmp/ldtoypad.prx /tmp/ldtoypad.sprx
```

---

## 2. Defect Resolution Summary

### Defect 1 & 5: Instruction Overflow and Branch Constraints — RESOLVED

**Executive Summary:**
The `make install` hook subsystem (`hook.c` + `hook.h`) has been **removed entirely**.
Instead, the SPRX builds **asm trampolines locally** using `sys_memory_allocate`
(which returns R-W-X pages) and writes 4-instruction `lis/ori/mtctr/bctr` patches.

**Resolution:**
1. `usb_hooks.c` calls `sys_memory_allocate(64KB, 64K)` to get an executable page.
2. 4 × 32-byte blocks are carved from this page (one per hook function).
3. Each block copies the **original 4 instructions** from the game's .text, then
   appends a 4-instruction branch-back (`lis r11, hi16 / ori r11, lo16 / mtctr
   r11 / bctr`).
4. The **patch preamble** (written by Node.js orchestrator via PS3MAPI
   `/write_process`) writes a 4-instruction `lis/ori/mtctr/bctr` over the game's
   first 16 bytes at each target address. This respects the R-X segment constraint
   by using Ring 0 writes via LV2 syscall.
5. The trampolines are **within ±32MB** of the SPRX's code segment (both allocated
   by the same `sys_memory_allocate`), so the assembly `bl` instructions in
   `toc_trampoline.s` work correctly.

**Files:**
- `sprx-plugin/usb_hooks.c` — `allocate_trampolines()`, trampoline setup loop
- `sprx-plugin/toc_trampoline.s` — Assembly wrappers, `call_original_*` stubs
- `sprx-plugin/toc_trampoline_c.c` — OPD resolution helpers

---

### Defect 2: Table of Contents (TOC) Register Corruption — RESOLVED

**Executive Summary:**
Direct branching from the game → C hook overwrites `r2` with the PRX TOC.
This is resolved by routing all entry points through assembly wrappers in
`toc_trampoline.s` that save/restore `r2`.

**Resolution:**
The assembly file `toc_trampoline.s` generates wrappers using the `HOOK_WRAPPER`
macro. Each wrapper:

1. Allocates a 0x60-byte stack frame (`stwu r1, -0x60(r1)`)
2. Saves LR (`mflr r0 / stw r0, 0x64(r1)`)
3. Saves the game's `r2` (`stw r2, 0x28(r1)`)
4. Calls the C hook function via `bl` (Sony SDK loads PRX TOC into `r2`)
5. Restores the game's `r2` (`lwz r2, 0x28(r1)`)
6. Restores LR and returns

**Files:**
- `sprx-plugin/toc_trampoline.s` — `HOOK_WRAPPER` macro + 4 wrapper instances
- `sprx-plugin/toc_trampoline_c.c` — `get_wrapper_*_addr()` helpers (OPD-aware)

---

### Defect 3: Global Offset Table (GOT) Resolution Failure — RESOLVED

**Executive Summary:**
The NID scanner now correctly emulates the `lis/ori` instruction sequence to
reconstruct the GOT slot address before dereferencing.

**Resolution:**
The scanner iterates over NID stubs in 3-word chunks (NID, reserved, GOT pointer).
It validates addresses (0x00010000 < addr < 0x4FFFFFFF), dereferences the GOT slot,
and **rejects PLT stubs** (addresses < 0x30000000). The `usb_hook_init` loop retries
up to 10 times (20 seconds max) with 2-second sleeps to wait for lazy binding
resolution.

**Key commentary in code:**
```c
/* REJECT PLT STUBS (CellOS lazy binding):
 * System PRXs (like cellUsbd) are always mapped at >= 0x30000000.
 * If we see an address below 0x30000000, it's a PLT stub — the
 * lazy binding hasn't fired yet. We must reject it and retry. */
```

**Files:**
- `sprx-plugin/usb_hooks.c` — `scan_for_nid()`, `find_game_cellusbd_functions()`
- `sprx-plugin/usb_hooks.c` — retry loop in `usb_hook_init()`

---

### Defect 4: Passthrough TOC Collisions — RESOLVED

**Executive Summary:**
Passthrough calls (non-ToyPad USB devices) no longer corrupt the game's `r2`.
The assembly stubs `call_original_*` restore the game's TOC before branching back.

**Resolution:**
The `call_original_*` assembly stubs in `toc_trampoline.s` receive the game's
original TOC (saved by the wrapper) as an argument from the C hook. They restore
`r2` before jumping into the trampoline (which contains the original game
instructions + branch-back).

The C hook functions now take `uint32_t game_toc` as their **last** argument
(not first), keeping the ABI compatible with standard calling conventions.

---

## 3. Current Source Tree

### New Files Created

| File                          | Purpose                                                   |
|-------------------------------|-----------------------------------------------------------|
| `sprx-plugin/toc_trampoline.s`  | PowerPC assembly: TOC wrappers + call_original stubs      |
| `sprx-plugin/toc_trampoline_c.c`| OPD resolution helpers for wrapper addresses              |
| `docs/HANDOFF-2026-07-20-RESOLVED.md` | This document                                      |

### Modified Files

| File                          | Changes                                                    |
|-------------------------------|------------------------------------------------------------|
| `sprx-plugin/usb_hooks.c`      | Removed `hook.h` include; OPD-based imports; PLT retry;    |
|                                | `sys_memory_allocate` trampolines; IPC file writer         |
| `sprx-plugin/network.c`        | Poll-based IP acquisition; removed unsupported SO_RCVTIMEO |
| `sprx-plugin/Makefile`         | Added `toc_trampoline_c.c`, `toc_trampoline.s` to build    |
|                                | Fixed signing step for space-in-path compatibility         |
| `sprx-plugin/build-all.ps1`    | Updated to include trampoline assembly                     |

### Source File Inventory

| File                          | Purpose                                                   |
|-------------------------------|-----------------------------------------------------------|
| `main.c`                       | PRX entry point: calls init/shutdown                      |
| `compat.c`                     | Sony SDK compatibility shims                              |
| `network.c`                    | UDP socket init, discovery, send/recv                     |
| `network.h`                    | Network state struct, packet types                        |
| `debug.c` / `debug.h`         | Logging subsystem                                         |
| `toypad_state.c` / `toypad_state.h` | USB descriptor state machine                        |
| `usb_hooks.c` / `usb_hooks.h` | NID scanner, trampolines, C hook functions                |
| `toc_trampoline.s`            | Assembly: TOC wrappers, call_original stubs               |
| `toc_trampoline_c.c`          | C helpers: OPD-resolved address getters                   |
| `hook.c` / `hook.h`           | **REMOVED from build** (no longer used)                   |
| `syscall.h`                   | LV2 syscall definitions (future use)                      |
| `ldd_driver.c` / `ldd_driver.h` | LDD driver (future use)                                |

---

## 4. Architecture: Hook Flow

```
Game calls:   cellUsbdInit@0x00101234
                    │
                    │ (4-instruction patch written by Node.js)
                    │   lis r11, hi16(wrapper_addr)
                    │   ori r11, r11, lo16(wrapper_addr)
                    │   mtctr r11
                    │   bctr
                    ▼
          wrapper_my_cellUsbdInit (toc_trampoline.s)
                    │
                    │ 1. stwu r1, -0x60(r1)
                    │ 2. Save LR
                    │ 3. **Save game's r2** ← CRITICAL
                    │ 4. bl my_cellUsbdInit (C function)
                    ▼
          my_cellUsbdInit (usb_hooks.c)
                    │
                    │ Returns CELL_OK (intercepted) OR
                    │ passthrough → call_original_*(game_toc, ...)
                    ▼
          call_original_BackToGame (toc_trampoline.s)
                    │
                    │ **Restore game's r2** ← CRITICAL
                    │ Jump into trampoline (at tramp_init_addr)
                    ▼
          Trampoline (allocated via sys_memory_allocate)
                    │
                    │ Original 4 instructions from game
                    │ lis/ori/mtctr/bctr → target+16
                    ▼
          cellUsbdInit+0x10 (game code resumes)
```

---

## 5. Node.js Orchestrator Integration

The SPRX writes IPC files to `/dev_hdd0/tmp/` which the Node.js orchestrator
reads via HTTP GET over webMAN MOD.

### IPC Files

| File                              | Written By    | Contents                                |
|-----------------------------------|---------------|-----------------------------------------|
| `/dev_hdd0/tmp/ld_hooks_ready.txt` | `usb_hooks.c` | `STATUS=ready`, target+wrapper addresses |
| `/dev_hdd0/tmp/ld_self_ip.txt`    | `network.c`   | `SELF_IP=192.168.0.47`                  |
| `/dev_hdd0/tmp/ld_recv_papertrail.txt` | `network.c` | Receive event log (first 10 packets)    |
| `/dev_hdd0/tmp/ld_hooks_shutdown.txt` | `usb_hooks.c` | `STATUS=shutdown`                     |

### Node.js Workflow

1. **Inject SPRX** via `/ps3mapi_process?pid=&load_prx=` (webMAN MOD API)
2. **Poll** `/dev_hdd0/tmp/ld_hooks_ready.txt` until `STATUS=ready`
3. **Parse** target addresses from IPC file
4. **Write 4-instruction patch** preamble via `/ps3mapi_process?pid=&write_process=`
   (Ring 0 — bypasses R-X segment protection)
5. **Begin server loop** — handle ToyPad data

---

## 6. Build Pipeline

### Prerequisites
- Sony Cell SDK 3.40 (DUPLEX) installed at `c:\usr\local\cell`
- oscetool in WSL at `~/oscetool-src/`
- PS3 with webMAN MOD + FTP server (credentials: mike/mike)

### Build Steps
```bash
# Option 1: Makefile (WSL)
cd '/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin'
make clean && make

# Option 2: PowerShell
.\sprx-plugin\build-all.ps1

# Deploy
.\ftp-deploy.ps1
```

### Makefile Targets
| Target     | Description                                  |
|------------|----------------------------------------------|
| `all`      | Build PRX + sign SPRX                        |
| `clean`    | Remove obj/ and build/ directories           |
| `rebuild`  | Clean + all                                  |

### Header Dependencies
- `sys/socket.h`, `netinet/in.h`, `arpa/inet.h` — BSD sockets (from `-lnet_stub`)
- `sys/timer.h` — `sys_timer_usleep()` (from `-llv2_stub`)
- `sys/sys_time.h` — `sys_time_get_system_time()` (from `-llv2_stub`)
- `sys/memory.h` — `sys_memory_allocate()` (from `-llv2_stub`)
- `cell/cell_fs.h` — `cellFsOpen/Write/Close` (from `-lfs_stub`)
- `netex/net.h` — `sys_net_initialize_network()` (from `-lnet_stub`)

**IMPORTANT:** These are **Sony SDK headers only**. PSL1GHT headers are strictly
prohibited. The SDK is at `c:\usr\local\cell\target\ppu\include`.

---

## 7. Deleted Files

- `sprx-plugin/hook.c` — **Removed from build** (Makefile no longer includes it)
- `sprx-plugin/hook.h` — **Removed from build** (no `#include "hook.h"` in any source)

The old `hook_install`/`hook_remove` subsystem is replaced by:
- `sys_memory_allocate` for trampoline pages
- Node.js orchestrator for Ring 0 patch preamble writing
- Direct assembly wrappers for TOC management

---

## 8. Network Stack Notes

### SO_RCVTIMEO — NOT SUPPORTED
CellOS SDK 3.40 does not support `struct timeval` / `SO_RCVTIMEO`. The network
mutex avoidance (sceNpTrophy deadlock prevention) is handled by `SO_NBIO`
(non-blocking mode), which causes `recvfrom()` to return `-1/EWOULDBLOCK`
immediately when no data is available. The calling thread yields control back
to the OS scheduler between poll cycles.

### Discovery
- Subnet broadcast `192.168.0.255` (not `255.255.255.255`)
- Packet type `0xF0` (NET_PACKET_TYPE_DISCOVERY)
- Self-rejection via `getsockname()` IP comparison
- 10-beacon startup salvo + periodic 250ms probes
- Server lock-on via packet type filtering (non-0xF0 packets ignored until
  server_known)
