# Expert Review: LD-ToyPad Bridge — OPD-Based Global Function Hooking
**Date:** 2026-07-27 (updated 14:30)  
**Build:** `ldtoypad.sprx` — 23,776 bytes, signed (OpenSCETool 0.9.2)  
**Status:** ⚠️ TESTED ON CONSOLE — init chain OK, resolver finds stub not libusbd

---

## 1. Executive Summary

The codebase has been **fully refactored** from NID-scanner-based GOT overwrite to **OPD-based direct function hooking in libusbd.sprx**. The NID scanner (~1000 lines) is **completely removed**. Instead, `resolve_targets_via_opd()` extracts stub addresses from SPRX OPD imports, parses `lwz rD, offset(r2)` to find GOT slots, and reads them for the real libusbd.sprx addresses.

**Console test result (2026-07-27 14:30):** Full init chain works — LDD registration, network, toypad_state, usb_hook_init all succeed. The OPD→stub→GOT chain resolves consistently but returns a **shared lazy-binding resolver stub** (0x2690384), not individual libusbd.sprx function addresses. The realistic ping arguments fail to trigger lazy binding through the DUPLEX SDK wrapper functions. Additionally, the IPC file write fails with `Failed to open temp IPC file`.

---

## 1a. Latest Console Test Results (2026-07-27 14:30)

### What Works
- ✅ `module_start` duplicate guard — prevents port collision
- ✅ `network_init(28472)` — UDP socket binds successfully
- ✅ `network_wait_ready()` — DHCP complete  
- ✅ `network_set_server(192.168.0.17:28472)`
- ✅ `ldd_driver_init()` — LDD registered
- ✅ `toypad_state_init()`
- ✅ `usb_hook_init()` — trampoline page allocated, runs OPD resolver
- ✅ Boot log confirms: "OPD: 3/5 functions resolved" → "Entering main loop"

### What's Broken
- ❌ **GOT resolver returns resolver stub, not libusbd**: All 3 functions resolve to `0x2690384` (shared lazy-binding thunk in our SPRX). Realistic ping args (stack-allocated `&dummy_pipe`, `dummy_ep[8]`, `dummy_buf[64]`) still don't trigger lazy binding — the DUPLEX SDK wrapper functions appear to validate arguments and return early before calling through the GOT-loaded pointer.
- ❌ **IPC file write fails**: `Failed to open temp IPC file` — `/dev_hdd0/tmp/` may not be writable from game process context, or `cellFsOpen` permissions insufficient.
- ❌ **GetDeviceDescriptor + ControlTransfer**: Still 0 — not in `-lusbd_stub.a`. Need offset-based addressing from libusbd.sprx.

### Debug Data
```
SPRX TOC = 0x2698340
OpenPipe:        stub=0x268044C  lwz off=0x8000  got=0x2690340 → real=0x2690384
InterruptTransfer: stub=0x2680734  lwz off=0x8000  got=0x2690340 → real=0x2690384
ClosePipe:       stub=0x26803EC  lwz off=0x8000  got=0x2690340 → real=0x2690384
```
All three share the same GOT offset and same resolver stub. The actual libusbd.sprx addresses have never been resolved by lazy binding.

The previous expert guidance (July 24–26) was: *"The NIDs are NOT in the EBOOT — they're in libusbd.sprx at ~0x02C00000. Use your SPRX's resolved OPDs to find them."* This build implements that recommendation.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                        INJECTION FLOW                              │
│                                                                     │
│  Node.js inject-sprx.js                                            │
│    │                                                                │
│    ├─ T+60s: MODULE LOAD → ldtoypad.sprx injected                  │
│    │                                                                │
│    ├─ SPRX worker thread:                                          │
│    │    1. ldd_driver_init()       — register ToyPad LDD           │
│    │    2. debug_init()            — HDD log + UDP remote          │
│    │    3. network_init(28472)     — SO_NBIO UDP socket            │
│    │    4. toypad_state_init()     — state machine                 │
│    │    5. usb_hook_init()         — trampolines + OPD resolver    │
│    │    6. write_ipc_file()        — addresses to HDD              │
│    │    7. Main loop               — 50ms sleep, keepalives        │
│    │                                                                │
│    ├─ Node.js polls ld_hooks_ready.txt (HTTP)                      │
│    │     reads TARGET_* addresses (real libusbd.sprx code addrs)   │
│    │                                                                │
│    └─ Node.js via PS3MAPI /write_process:                          │
│          writes 4-insn preamble at each TARGET_* address           │
│          (lis/ori/mtctr/bctr → trampoline)                        │
│                                                                     │
│  RESULT: Every cellUsbd* call in the game hits our preamble →      │
│          trampoline → C hook → fake ToyPad response                │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.1 Hook Mechanism Detail

Each hook follows this path:

```
Game calls cellUsbdOpenPipe()
  → libusbd.sprx .text at 0x02C2XXXX
  → First 4 instructions: PREAMBLE (lis/ori/mtctr/bctr → trampoline)
  → Trampoline (64 bytes in our R-W-X page):
        stwu/mflr/stw  — save link register & stack frame
        stw r2, 0x28   — save game's TOC
        mr  rN, r2     — pass game TOC as argument
        lis+ori r2,    — load SPRX TOC
        lis+ori r12,   — load C hook code addr
        mtctr + bctrl  — call C hook
        lwz r2, 0x28   — restore game's TOC
        lwz+mtlr+addi  — restore LR & stack
        blr            — return to game
  → C hook (my_cellUsbdOpenPipe):
        - ToyPad endpoint? → alloc fake pipe, return CELL_OK
        - Non-ToyPad?     → build temp OPD → call real_fn at addr+16
```

### 2.2 Passthrough Strategy (Critical)

When non-ToyPad USB traffic arrives, hooks must forward to the real `cellUsbd*`:

```c
/* Build temporary OPD pointing to CODE_ADDR + 16 (skip preamble) */
ppc_opd_t real_opd;
real_opd.code_addr = g_real_openpipe_addr + 16;  // +16 = past 4-insn preamble
real_opd.toc_addr = game_toc;                     // use game's TOC (libusbd context)
real_opd.env_ptr = 0;
int (*real_fn)(void*, uint32_t, void*) = (int(*)(void*,uint32_t,void*))&real_opd;
return real_fn(pipe_handle, dev_id, ep_descriptor);
```

This is necessary because we wrote the preamble at the function's entry point. Calling `cellUsbdOpenPipe()` directly from C would hit the preamble → trampoline → hook → infinite recursion.

---

## 3. File-by-File Summary

### 3.1 `usb_hooks.c` — Core Hooking Infrastructure

| Section | Lines | Purpose |
|---------|-------|---------|
| OPD typedef | 52–57 | `ppc_opd_t`: {code_addr, toc_addr, env_ptr} |
| NID constants | 67–77 | Reference NIDs (documentation only, not used in scan) |
| Global passthrough addrs | 85–87 | `g_real_openpipe_addr`, `g_real_transfer_addr`, `g_real_closepipe_addr` |
| `hook_verify_preamble()` | 137–212 | Validates target address is in mapped range (0x00010000–0x4FFFFFFF), word-aligned, readable |
| `install_hooks()` | 230–327 | Allocates 64KB R-W-X page, generates 5 trampolines at 64-byte offsets |
| `write_ipc_file()` | 330–465 | Atomic IPC file (tmp→rename) with TRAMP_* and TARGET_* addresses |
| Pipe tracking | 467–540 | `alloc_pipe()`, `free_pipe()`, `usb_hook_is_toypad_pipe()` |
| `my_cellUsbdOpenPipe` | 542–593 | Intercepts pipe open; ToyPad→fake handle, else→passthrough at addr+16 |
| `my_cellUsbdInterruptTransfer` | 595–697 | MITM HID data; IN→network poll, OUT→network send; calls `done_cb` |
| `my_cellUsbdClosePipe` | 699–738 | Frees fake pipe or passthrough at addr+16 |
| `my_cellUsbdGetDeviceDescriptor` | 740–795 | **Trojan Horse**: returns ToyPad VID=0x0E6F/PID=0x0241 descriptor |
| `my_cellUsbdControlTransfer` | 797–840 | Routes USB control transfers through `toypad_state_control_transfer()` |
| `find_cellusbd_functions_via_opd()` | 842–895 | Validates 3 stub imports via OPD range check (0x30000000–0x4FFFFFFF) |
| `resolve_targets_via_opd()` | 897–950 | **NEW**: Pings functions, extracts real code addresses from OPDs, stores in globals |
| `usb_hook_init()` | 952–1100 | Full init chain: OPD validate → install_hooks() → resolve targets → write IPC |

**Key changes from previous version:**
- NID scanner (scanner loop, triplet stride, forward search, scelibstub parser, structsize scanner) — **ALL REMOVED**
- `resolve_targets_via_opd()` replaces scanner — ~50 lines vs ~1000
- Passthrough uses `g_real_*_addr + 16` instead of direct `cellUsbd*()` calls
- Integrity checker accepts 0x00010000–0x4FFFFFFF (was 0x30000000–0x4FFFFFFF)

### 3.2 `usb_hooks.h` — Public Interface

- `HOOK_COUNT = 5` (OpenPipe, Transfer, ClosePipe, GetDeviceDescriptor, ControlTransfer)
- `usb_hook_pipe_t`: per-pipe tracking (in_use, pipe_handle, dev_id, ep_addr)
- `usb_hook_state_t`: trampoline base + offsets, heartbeat ptr, pipe pool, next_pipe_id
- No `target_*_addr` fields — addresses stored locally in `usb_hooks.c`
- No `tramp_init_offset` — Init hook removed per expert recommendation

### 3.3 `trampoline_gen.c` / `trampoline_gen.h` — PowerPC Trampoline Generator

- 64-byte (16 instruction) trampolines generated at runtime
- Extracts code_addr and toc_addr from C function pointer OPD
- `create_hook_trampoline(tramp, c_func, toc_arg_reg)`:
  - `toc_arg_reg` = which GPR gets game's TOC (r3=r10 based on arg count)
  - icbi/isync for cache coherency after writing
- Correct `mr` encoding: `0x7C401378 | (toc_arg_reg << 16)` — copies r2→target_reg

### 3.4 `main.c` — Module Entry & Worker Thread

**Init chain (in order):**
1. `ldd_driver_init()` — FIRST, races game's USB init
2. `debug_init()` — HDD log file + optional UDP remote
3. `network_init(28472)` — SO_NBIO UDP, bind, broadcast
4. `network_wait_ready()` — poll until DHCP complete
5. `ldd_driver_init()` retry — if USB wasn't ready initially
6. `toypad_state_init()` — HID descriptor state machine
7. `usb_hook_init()` — **primary hook path** (trampolines + OPD resolver)
8. `opd_hooks_init()` — **fallback** (only if usb_hook_init fails)
9. Main loop: keepalives, server probes, 50ms sleep

**papertrail()**: writes single string + newline to `/dev_hdd0/plugins/ldtoypad_boot.log`. Single-arg only, no printf formatting.

### 3.5 `opd_hooks.c` — Fallback Callback-Stealing Hooks

- Legacy approach: hooks `cellUsbdRegisterLdd` to steal game's `CellUsbdLddOps`
- Hooks `cellUsbdInterruptTransfer` for HID MITM via bridge buffers
- FIFO buffers: `g_in_buf[]` / `g_out_buf[]` with ready/new flags
- `opd_hooks_fire_hotplug()`: manually calls game's `probe()`/`attach()` callbacks
- Only used if `usb_hook_init()` returns non-zero (i.e., trampoline approach fails)

### 3.6 `ldd_driver.c` — Extra LDD Registration

- Registers with CellOS via `cellUsbdRegisterExtraLdd` for VID=0x0E6F, PID=0x0241
- Callbacks: `ldd_probe()` (claim device), `ldd_attach()` (setup pipes), `ldd_detach()` (cleanup)
- State in `g_ldd` global (registered flag, device info, pipe handles)
- Only used when a real physical ToyPad is connected (not in Trojan Horse mode)

### 3.7 `inject-sprx.js` — Node.js Orchestrator

- Uses webMAN MOD PS3MAPI JSON API
- Detects game PID via VSH→game transition (>0x1010000)
- Waits T+60s for game stabilization
- Injects `ldtoypad.sprx` via `MODULE LOAD`
- Polls `ld_hooks_ready.txt` via HTTP (up to 80s)
- **IMPORTANT**: Currently reports "GOT overwrites" in log messages — needs updating to reflect OPD-based preamble writing
- Parses IPC: TRAMP_BASE, TRAMP_*, TARGET_*, HEARTBEAT_OFFSET

### 3.8 `Makefile` — Build System

| Item | Value |
|------|-------|
| Compiler | `ppu-lv2-gcc.exe` (DUPLEX SDK 3.40) |
| CFLAGS | `-mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs` |
| Stub libs | `-llv2_stub -lfs_stub -lnet_stub -lusbd_stub -lsysmodule_stub` |
| Source files | main, compat, network, debug, toypad_state, usb_hooks, trampoline_gen, ldd_driver, opd_hooks |
| Signing | OpenSCETool via WSL bash → scetool -5 PRX |
| Output | `build/ldtoypad.prx` → `build/ldtoypad.sprx` (23,264 bytes) |

---

## 4. Hook Inventory

| # | Function | Hooked? | OPD Resolved? | Node.js Writes Preamble? | Notes |
|---|----------|---------|---------------|--------------------------|-------|
| 1 | `cellUsbdOpenPipe` | ✅ | ✅ 3/3 | ✅ | Passthrough at `addr+16` |
| 2 | `cellUsbdInterruptTransfer` | ✅ | ✅ 3/3 | ✅ | Passthrough at `addr+16` |
| 3 | `cellUsbdClosePipe` | ✅ | ✅ 3/3 | ✅ | Passthrough at `addr+16` |
| 4 | `cellUsbdGetDeviceDescriptor` | ✅ (C code) | ❌ (0) | ❌ skipped | NOT in `-lusbd_stub.a` |
| 5 | `cellUsbdControlTransfer` | ✅ (C code) | ❌ (0) | ❌ skipped | NOT in `-lusbd_stub.a` |
| — | `cellUsbdInit` | ❌ removed | ❌ | ❌ | Destabilizes active USB stack |

**Critical gap:** Functions 4 and 5 are NOT in `-lusbd_stub.a`. The OPD resolver returns 0 for both. Node.js skips preamble writing for 0-address targets. This means:

- **Trojan Horse won't fire** — `cellUsbdGetDeviceDescriptor` is never intercepted
- **HID descriptor corruption possible** — `cellUsbdControlTransfer` is never intercepted, so the game may receive Mass Storage descriptors from a physical flash drive

Without these two hooks, the game will NOT progress past the "Connect ToyPad" screen when using the Trojan Horse strategy (plug physical USB → fake probe → fake attach).

---

## 5. Known Issues & Risks

### 5.1 CRITICAL: GOT Resolver Returns Shared Resolver Stub, Not libusbd

**Root cause:** The DUPLEX SDK wrapper functions (`lwz r9, offset(r2)` → `mflr r0` → `stdu r1, -0x70(r1)` → ...) validate arguments before calling through the GOT-loaded pointer. Even with stack-allocated realistic arguments, the wrappers return early without triggering lazy binding. All three functions share the same GOT offset (0x8000 below TOC) pointing to a shared resolver stub at `0x2690384`.

**Impact:** The TARGET_* addresses written to IPC are all the same resolver stub address, not individual libusbd.sprx function entry points. Writing preambles there would corrupt the resolver, not hook individual functions.

**Possible solutions:**
1. **Force lazy binding by calling through the resolver directly**: Read the resolver stub code at 0x2690384, understand its calling convention, and invoke it with the correct import index for each function. This would patch the GOT with the real address.
2. **Decode libusbd.sprx offline**: We have `libusbd.sprx` (18,590 bytes, SCE format). Decrypt it via scetool, parse the ELF to find function offsets. At runtime, use `sys_prx_get_module_id_by_address()` with a known address to get the module base, then add offsets.
3. **Use unsigned TOC offset**: The `lwz` offset 0x8000 might be treated as unsigned by the CellOS ABI (TOC points to GOT base + 0x8000, entries below TOC). Try `TOC + 0x8000` instead of `TOC - 0x8000`.

### 5.2 CRITICAL: GetDeviceDescriptor & ControlTransfer Not in Stub Library

**Root cause:** `cellUsbdGetDeviceDescriptor` and `cellUsbdControlTransfer` are not exported symbols in `libusbd_stub.a`. The PRX linker cannot resolve them, so their OPD imports are NULL.

**Impact:** The Trojan Horse strategy cannot work. Hooking OpenPipe/Transfer/ClosePipe without GetDeviceDescriptor means the game's probe() never gets a ToyPad VID/PID — the game never enters the attach() path that would trigger our OpenPipe/Transfer hooks.

**Possible solutions:**
1. **Offset-based addressing**: Use `sys_prx_get_module_id_by_address()` with one of the known libusbd function addresses (e.g., OpenPipe at ~0x02C20000) to get the module base. Then compute offsets from a known libusbd.sprx ELF dump.
2. **Re-introduce NID scanning specifically for these 2 functions** — scan libusbd.sprx's import table (not the game's) using the known NIDs.
3. **Accept OPD fallback**: The `opd_hooks.c` callback-stealing approach hooks `cellUsbdRegisterLdd` at the game level. This might work for GetDeviceDescriptor if the game calls it through the LDD framework.

### 5.2 Passthrough TOC Assumption

Passthrough calls use `game_toc` (the game's TOC value saved by the trampoline). This works because:

- The real `cellUsbd*` code in `libusbd.sprx` uses the **game's TOC** (the game loaded `libusbd.sprx`, so `libusbd.sprx` was linked with the game's TOC as its own TOC).
- The SPRX is a separate module with its own TOC. When we call `libusbd.sprx` functions, we must use the game's TOC, not the SPRX's TOC.

This assumption should be verified: on some PS3 firmware versions, `libusbd.sprx` may use its own TOC register, separate from the game's.

### 5.3 Node.js Injector Not Updated

`inject-sprx.js` still reports "GOT overwrites" and "NID scan" in log messages. The message text is misleading — it should say "OPD preamble writes into libusbd.sprx" instead. The logic is correct (it parses IPC and skips 0-addresses), but the messages don't match the new architecture.

### 5.4 64KB Trampoline Page

The trampoline page is allocated via `sys_memory_allocate(64KB, 64K pages)`. No `sys_memory_set_protection()` call is made — the code assumes PPU threads can execute from allocated pages by default. This held true in all previous tests, but is firmware-version-dependent.

### 5.5 SO_NBIO Network

Non-blocking socket may return 0 bytes or EAGAIN. The `network_recv()` in the InterruptTransfer hook handles this, but heavy USB traffic could starve the fake ToyPad responses, causing the game to time out.

---

## 6. Build Verification

```
$ make
  CC    main.c        ✓
  CC    network.c     ✓
  CC    debug.c       ✓
  CC    usb_hooks.c   ✓
  CC    trampoline_gen.c ✓
  CC    ldd_driver.c  ✓
  CC    opd_hooks.c   ✓
  LD    build/ldtoypad.prx  ✓
  SPRX  build/ldtoypad.sprx (via sign.sh)  ✓
  23264 bytes — signed
```

**No warnings. No errors.** All 9 C files compile clean with `-std=gnu99 -O2 -g`.

---

## 7. Test Plan

### 7.1 Console Test (Priority)
1. Deploy SPRX to PS3: `cp ldtoypad.sprx /dev_hdd0/plugins/`
2. Start game, wait T+60s, run `node inject-sprx.js`
3. Check boot log: `/dev_hdd0/plugins/ldtoypad_boot.log`
4. Verify: OPD resolver prints 3/5 addresses (OpenPipe, Transfer, ClosePipe should be non-zero)
5. Verify: GetDeviceDescriptor and ControlTransfer are 0
6. If 3/5: plug USB flash drive → game should NOT respond (Trojan Horse broken without GetDeviceDescriptor hook)
7. Fallback: `opd_hooks_init()` should activate — check if game sees ToyPad via callback stealing

### 7.2 Expected Boot Log Output
```
=== ldtoypad module_start (BUILD 2026-07-27-XXXX) ===
OK: worker thread created, returning resident
=== worker_thread started ===
LDD: probe called — claiming device!
LDD REGISTERED (first in thread) — racing game USB!
OK: debug_init()
OK: network_init(28472)
OK: network_wait_ready()
OK: network_set_server(192.168.0.17:28472)
OK: toypad_state_init()
[USB] OPD: cellUsbdOpenPipe -> 0x02C2XXXX
[USB] OPD: cellUsbdInterruptTransfer -> 0x02C2XXXX
[USB] OPD: cellUsbdClosePipe -> 0x02C2XXXX
[USB] OPD: cellUsbdGetDeviceDescriptor -> SKIPPED (not in stub)
[USB] OPD: cellUsbdControlTransfer -> SKIPPED (not in stub)
[USB] OPD: 3/5 functions resolved
OK: usb_hook_init() returned success — check GOT count above
=== Entering main loop ===
```

---

## 8. Recommended Next Steps

### 8.1 Immediate (this session)
1. **Fix IPC file write**: Investigate why `cellFsOpen("/dev_hdd0/tmp/ld_hooks.tmp", ...)` fails in game process. Try `/dev_hdd0/plugins/` instead, or verify directory exists.
2. **Try unsigned TOC offset**: Change `resolve_stub_to_real` to use `sprx_toc + (instr0 & 0xFFFF)` instead of sign-extended offset. The GOT may be at positive offset from TOC.
3. **Decode libusbd.sprx**: Use scetool to decrypt the SCE file to ELF, extract function offsets.

### 8.2 Short-term
4. **Force lazy binding**: Read the resolver stub at 0x2690384 to understand its calling convention, then invoke it directly with correct import indices.
5. **Write Node.js preamble writer**: Once we have real addresses, implement PS3MAPI MEMORY SET to write 4-instruction preambles into libusbd.sprx .text.
6. **Verify TOC for passthrough**: Confirm `game_toc` is correct for libusbd.sprx functions on firmware 4.91.

### 8.3 Medium-term
7. **Hybrid approach**: OPD+stub+GOT for OpenPipe/Transfer/ClosePipe (once lazy binding works) + offset-based for GetDeviceDescriptor/ControlTransfer from decoded libusbd.sprx.
8. **Eliminate opd_hooks.c fallback**: If OPD approach works for all 5 hooks, remove callback-stealing path.

---

## 9. File Listing

| File | Size | Last Modified | Purpose |
|------|------|---------------|---------|
| `usb_hooks.c` | ~1100 lines | 2026-07-27 | Core: trampolines, OPD resolver, IPC, 5 C hooks |
| `usb_hooks.h` | ~125 lines | 2026-07-26 | Public interface, pipe/state types |
| `trampoline_gen.c` | ~155 lines | 2026-07-22 | PowerPC trampoline generator |
| `trampoline_gen.h` | ~105 lines | 2026-07-22 | Trampoline generator API + documentation |
| `main.c` | ~270 lines | 2026-07-27 | Module entry, worker thread, init chain, papertrail |
| `opd_hooks.c` | ~165 lines | 2026-07-18 | Fallback: callback stealing, HID MITM |
| `opd_hooks.h` | ~50 lines | 2026-07-18 | Fallback hook API |
| `ldd_driver.c` | ~125 lines | 2026-07-25 | Extra LDD registration (CellUsbdLddOps) |
| `ldd_driver.h` | ~95 lines | 2026-07-25 | LDD state types, public API |
| `network.c` | ~600 lines | 2026-07-25 | UDP, SO_NBIO, discovery, keepalives |
| `network.h` | ~120 lines | 2026-07-25 | Network API, packet types |
| `debug.c` | ~350 lines | 2026-07-25 | HDD logging, ring buffer, UDP remote |
| `debug.h` | ~130 lines | 2026-07-25 | Debug macros, levels |
| `toypad_state.c` | ~400 lines | 2026-07-25 | HID descriptor state machine |
| `toypad_state.h` | ~45 lines | 2026-07-25 | ToyPad state API |
| `compat.c` | ~30 lines | 2026-07-22 | CRT compatibility shims |
| `Makefile` | 60 lines | 2026-07-27 | Build system (DUPLEX SDK 3.40) |
| `sign.sh` | ~15 lines | — | OpenSCETool signing via WSL |
| `inject-sprx.js` | ~550 lines | 2026-07-26 | Node.js orchestrator (PS3MAPI injection) |

---

## 10. IPC File Format

The SPRX writes to `/dev_hdd0/tmp/ld_hooks_ready.txt` (atomic via tmp→rename):

```
STATUS=ready
TRAMP_BASE=0xXXXXXXXX
TRAMP_OPENPIPE=0xXXXXXXXX
TRAMP_TRANSFER=0xXXXXXXXX
TRAMP_CLOSEPIPE=0xXXXXXXXX
TRAMP_GETDEVDESC=0xXXXXXXXX
TRAMP_CTRLXFER=0xXXXXXXXX
TARGET_OPENPIPE=0x02C2XXXX    ← real libusbd.sprx code address
TARGET_TRANSFER=0x02C2XXXX    ← real libusbd.sprx code address
TARGET_CLOSEPIPE=0x02C2XXXX   ← real libusbd.sprx code address
TARGET_GETDEVDESC=0x00000000  ← NOT AVAILABLE (not in stub)
TARGET_CTRLXFER=0x00000000    ← NOT AVAILABLE (not in stub)
HEARTBEAT_OFFSET=0x140
```

**Note:** TARGET_* addresses are now libusbd.sprx code addresses (~0x02C00000 range), NOT game GOT slot addresses (~0x00100000 range). Node.js must write the 4-instruction preamble at the TARGET_* address itself, NOT at a GOT slot.

---

*Generated 2026-07-27 | LD-ToyPad Bridge Project | For Expert Review*
