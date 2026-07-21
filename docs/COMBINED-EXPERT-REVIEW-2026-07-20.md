# LD-ToyPad Bridge: Combined Expert Review & Corrected Implementation Plan
## 2026-07-21 — Expert-Corrected Architecture

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Expert Q&A: Corrected Understanding](#2-expert-qa-corrected-understanding)
3. [Phase 1: network_init() Fix — Corrected Plan](#3-phase-1-network_init-fix--corrected-plan)
4. [Phase 2: PRX Teardown Fix (Root Cause)](#4-phase-2-prx-teardown-fix-root-cause)
5. [Phase 3: Memory-Mapped Heartbeat (No HDD!)](#5-phase-3-memory-mapped-heartbeat-no-hdd)
6. [Phase 4: Sleep 10ms → 50ms for Trophy Safety](#6-phase-4-sleep-10ms--50ms-for-trophy-safety)
7. [Phase 5: Atomic IPC File Writing](#7-phase-5-atomic-ipc-file-writing)
8. [Phase 6: NID Scanner Memory Safety](#8-phase-6-nid-scanner-memory-safety)
9. [Phase 7: Build Deploy & Validate](#9-phase-7-build-deploy--validate)
10. [Complete Files to Modify](#10-complete-files-to-modify)

---

## 1. Executive Summary

**Current Status:** SPRX loads but fails at `network_init()`. PS3 papertrail proves `FAIL: network_init() — fatal` after 30×100ms retry exhaustion. All 5 hooking defects sit untested behind this blocker.

**Expert corrections (major changes from previous plan):**
1. ❌ **SO_REUSEADDR is a trap** — UDP has no TIME_WAIT. The port is held by the **orphaned thread from the previous SPRX injection** (no `module_stop` between loads). SO_REUSEADDR would cause nondeterministic packet routing. Fix: **explicit PRX unloading** via Node.js orchestrator before inject.
2. ❌ **HDD heartbeat is dangerous** — cellFsWrite in game process causes I/O contention that triggers the freeze we're debugging. Fix: **memory-mapped heartbeat** in sys_memory_allocate page, polled via PS3MAPI /read_process.
3. ❌ **10ms sleep starves sceNpTrophy** — 100Hz is too aggressive. Fix: **50ms (20Hz)**.
4. ✅ **120 retries is safe** — Detached worker thread, game continues.
5. ✅ **sys_memory_allocate IS executable (R-W-X)** — Confirmed.
6. ✅ **OPD toc_addr=0 is safe** — Sequential execution, no race.
7. ✅ **cellFsRename needed** — Atomic IPC writes.

---

## 2. Expert Q&A: Corrected Understanding

### Q1: Is SO_REUSEADDR available?
**Yes, but DO NOT USE IT.** UDP is connectionless — no TIME_WAIT state exists. The port is held open by the **orphaned socket of the previous SPRX worker thread** that never ran module_stop. SO_REUSEADDR would force-bind, but incoming packets would be nondeterministically routed between the ghost SPRX and your new SPRX, causing severe packet loss.

### Q2: Is 120 retries safe?
**Yes.** The worker thread is detached (`SYS_PPU_THREAD_CREATE_JOINABLE` but created at lower priority 1000). The game's main boot sequence is unaffected.

### Q3: Is ENETDOWN expected?
**Yes.** `0x80010041` (ENETDOWN) is normal during early boot. The retry loop correctly waits for interfaces to spin up.

### Q4: Is HDD heartbeat safe?
**No.** cellFsOpen/cellFsWrite in a game process causes severe I/O contention with the game's asset streaming and trophy sync. **Use memory-mapped heartbeat** instead.

### Q5: Is 10ms too aggressive?
**Yes.** Change to **50ms (20Hz)** — standard polling rate for PS3 peripheral emulation.

### Q6: Is sys_memory_allocate executable?
**Yes.** Default container (0xFFFFFFFF) does not enforce NX. Pages are fully R-W-X.

### Q7: OPD toc_addr=0 race?
**No race.** PowerPC execution is strictly sequential. The `mr %r2, %r3` in the assembly macro executes before any memory access — r2 is never read from the zeroed OPD.

### Q8: cellFsO_TRUNC race?
**Yes.** Truncation + read race can give Node.js a malformed IPC file. **Fix: write to .tmp, then cellFsRename to final name.**

### Q9: NID scan DSI risk?
**Yes.** Scanning high memory blindly risks DSIs from unmapped pages. **Constrain scan regions or dynamically resolve cellUsbd base address.**

---

## 3. Phase 1: network_init() Fix — Corrected Plan

### File: `sprx-plugin/network.c`

Changes:
1. **Add ENTRY debug log** at function start
2. **Add 2-second stabilization delay** before retry loop
3. **Extend retry 30→120** (12 seconds total)
4. **NO SO_REUSEADDR** (removed — expert says UDP has no TIME_WAIT)
5. **Fix `last_bind_err`** — store bind return value, not socket fd
6. **Add per-attempt logging** for socket and bind errors

```c
int network_init(uint16_t port)
{
    struct sockaddr_in local_addr;
    int ret;

    if (g_net.initialized) return 0;

    DEBUG_PRINT("[NET] network_init(port=%u) ENTRY\n", (unsigned)port);

    /* Initialize sys_net */
    ret = sys_net_initialize_network();
    if (ret != 0) {
        DEBUG_ERROR("[NET] sys_net_initialize_network failed: 0x%x\n", ret);
    }

    /* 2-second boot stabilization — let network interfaces spin up */
    sys_timer_usleep(2000000);

    /* Retry socket() + bind() up to 120 times (12 seconds at 100ms intervals).
     *
     * CRITICAL: DO NOT use SO_REUSEADDR. UDP has no TIME_WAIT state.
     * If bind fails, the port is held by the orphaned socket of a previous
     * SPRX injection that never ran module_stop. The fix is to explicitly
     * unload the PRX via Node.js orchestrator before injecting the new build.
     */
    {
        int attempt;
        int last_socket_err = 0;
        int last_bind_err = 0;
        for (attempt = 0; attempt < 120; attempt++) {
            g_net.socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (g_net.socket_fd < 0) {
                last_socket_err = g_net.socket_fd;
                DEBUG_PRINT("[NET] socket() failed attempt %d/%d: 0x%x\n",
                            attempt + 1, 120, last_socket_err);
                sys_timer_usleep(100000);
                continue;
            }

            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin_family = AF_INET;
            local_addr.sin_port = htons(port);
            local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

            int bind_ret = bind(g_net.socket_fd,
                                (struct sockaddr*)&local_addr,
                                sizeof(local_addr));
            if (bind_ret == 0) {
                break;  /* Success */
            }
            last_bind_err = bind_ret;  /* FIXED: was g_net.socket_fd (bug) */
            DEBUG_PRINT("[NET] bind() failed attempt %d/%d: %d\n",
                        attempt + 1, 120, bind_ret);
            socketclose(g_net.socket_fd);
            g_net.socket_fd = -1;
            sys_timer_usleep(100000);
        }
        if (g_net.socket_fd < 0) {
            DEBUG_ERROR("[NET] socket+bind failed after %d attempts "
                        "(last socket=0x%x, last bind=%d)\n",
                        120, last_socket_err, last_bind_err);
            return -1;
        }
        DEBUG_PRINT("[NET] socket()+bind() succeeded on attempt %d\n", attempt + 1);
    }
    // ... rest of init (SO_BROADCAST, SO_NBIO, discovery setup) ...
}
```

---

## 4. Phase 2: PRX Teardown Fix (Root Cause)

The **real root cause** of the `network_init()` regression is that the SPRX is **injected without first unloading the previous instance**. The orphaned worker thread still holds `g_net.socket_fd` open, so `bind()` fails with EADDRINUSE.

### Fix: Node.js orchestrator (`inject-sprx.js`)

Before injecting new SPRX, unload the old one:

```javascript
// Step 0: Unload previous PRX (if any)
const unloadUrl = ps3mapiBase + '?pid=' + gamePid + '&unload_prx=' + encodeURIComponent(prxPath);
await httpGet(unloadUrl);
await sleep(500);  // Wait for module_stop to complete

// Step 1: Load new PRX
const loadUrl = ps3mapiBase + '?pid=' + gamePid + '&load_prx=' + encodeURIComponent(prxPath);
// ...
```

### File: `sprx-plugin/main.c` — Ensure module_stop is robust

```c
int module_stop(void)
{
    papertrail("=== module_stop ===");
    g_shutdown = 1;

    if (g_worker_tid != SYS_PPU_THREAD_ID_INVALID) {
        uint64_t ev = 0;
        sys_ppu_thread_join(g_worker_tid, &ev);
        g_worker_tid = SYS_PPU_THREAD_ID_INVALID;
        papertrail("OK: worker joined");
    }

    // Close socket — releases port for next injection
    if (g_net.socket_fd >= 0) {
        socketclose(g_net.socket_fd);
        g_net.socket_fd = -1;
    }
    sys_net_finalize_network();

    toypad_state_deinit();
    usb_hook_shutdown();
    debug_shutdown();

    papertrail("=== module_stop SUCCESS ===");
    return SYS_PRX_STOP_OK;
}
```

---

## 5. Phase 3: Memory-Mapped Heartbeat (No HDD!)

### Problem
Writing heartbeat to HDD (`cellFsOpen`/`cellFsWrite`) once per second causes severe I/O contention with game asset streaming and trophy sync — potentially triggering the exact freeze we're trying to detect.

### Solution
Use the **already-allocated sys_memory_allocate page** as a memory-mapped heartbeat. The Node.js orchestrator polls it via PS3MAPI `/read_process` — zero HDD writes.

### Changes to `sprx-plugin/usb_hooks.h`

Add heartbeat offset to the global state:

```c
typedef struct {
    // ... existing fields ...
    uint32_t          tramp_init_addr;
    uint32_t          tramp_open_pipe_addr;
    uint32_t          tramp_transfer_addr;
    uint32_t          tramp_close_pipe_addr;
    
    /* Heartbeat counter — allocated in the same sys_memory_allocate page.
     * Located at offset 128 (after 4 × 32-byte trampoline blocks).
     * Incremented each worker loop iteration (~20 times/sec at 50ms sleep).
     * Polled by Node.js orchestrator via PS3MAPI /read_process.
     * Zero HDD writes — no I/O contention with game. */
    volatile uint32_t *heartbeat;   /* Pointer to heartbeat counter */
    
    // ... pipe tracking fields ...
} usb_hook_state_t;
```

### Changes to `sprx-plugin/usb_hooks.c`

In `allocate_trampolines()`, after allocating the 4 trampoline blocks:

```c
    /* Allocate heartbeat counter at offset 128 in the same page.
     * Zero-initialized by sys_memory_allocate (page is zeroed). */
    g_usb_hooks.heartbeat = (volatile uint32_t*)(uintptr_t)(base_addr + 128);
    
    // Export heartbeat address in IPC file for Node.js polling
```

### Changes to `sprx-plugin/main.c` — In worker_thread main loop:

```c
    uint8_t seq = 0;
    while (!g_shutdown) {
        uint8_t buf[NET_PACKET_MAX_SIZE];
        
        /* Memory-mapped heartbeat — incremented every iteration.
         * Polled by Node.js orchestrator via PS3MAPI /read_process.
         * At 50ms sleep, this increments at ~20 Hz.
         * No HDD writes — avoids I/O contention with game. */
        if (g_usb_hooks.heartbeat) {
            (*g_usb_hooks.heartbeat)++;
        }
        
        int n = network_recv(buf, sizeof(buf));
        if (n > 0) {
            DEBUG_VERBOSE("[MAIN] RX %d bytes from server\n", n);
        }
        network_maybe_probe_server(seq++);
        network_send_keepalive();
        
        /* 50ms yield (20 Hz) — prevents sceNpTrophy deadlock.
         * 10ms (100 Hz) was starving background threads. */
        sys_timer_usleep(50000);
    }
```

---

## 6. Phase 4: Sleep 10ms → 50ms for Trophy Safety

### File: `sprx-plugin/main.c` line 217

```c
// CHANGE FROM:
sys_timer_usleep(10000); /* 10ms — was 100Hz, too aggressive */
// CHANGE TO:
sys_timer_usleep(50000); /* 50ms — 20Hz, standard for peripheral emulation */
```

**Why:** The 10ms sleep caused the worker thread to wake 100 times/second. At this frequency, the PPU scheduler can starve the `sceNpTrophy` background threads of `sceNet` mutex access, causing the "Loading Trophy" freeze. 50ms (20Hz) leaves ample timeslices for OS tasks while still providing responsive network polling.

---

## 7. Phase 5: Atomic IPC File Writing

### File: `sprx-plugin/usb_hooks.c` — `write_ipc_file()`

Current code writes directly to `/dev_hdd0/tmp/ld_hooks_ready.txt` with `CELL_FS_O_TRUNC`. If Node.js polls during the write, it reads a truncated file.

**Fix:** Write to `.tmp`, then atomic rename:

```c
static int write_ipc_file(void)
{
    int fd;
    uint64_t written;
    char buf[512];
    int pos = 0;
    // ... (existing IPC data construction) ...

    /* First write to .tmp file — atomic rename prevents truncated reads.
     * Node.js polls for ld_hooks_ready.txt — it will only see the complete
     * file after cellFsRename succeeds. */
    if (cellFsOpen("/dev_hdd0/tmp/ld_hooks.tmp",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_OK) {
        DEBUG_ERROR("[USB] Failed to open tmp IPC file\n");
        return -1;
    }
    
    cellFsWrite(fd, buf, pos, &written);
    cellFsClose(fd);
    
    /* Atomic rename — if this crashes, Node.js sees stale (but valid) file,
     * never a truncated one. */
    if (cellFsRename("/dev_hdd0/tmp/ld_hooks.tmp",
                     "/dev_hdd0/tmp/ld_hooks_ready.txt") != CELL_OK) {
        DEBUG_ERROR("[USB] cellFsRename failed\n");
        return -1;
    }
    
    DEBUG_PRINT("[USB] IPC file written (%d bytes, atomic rename)\n", pos);
    return 0;
}
```

Also apply to shutdown IPC file in `usb_hook_shutdown()`:

```c
    // Atomic rename for shutdown file too
    if (cellFsOpen("/dev_hdd0/tmp/ld_shutdown.tmp", ...) == CELL_OK) {
        cellFsWrite(...);
        cellFsClose(fd);
        cellFsRename("/dev_hdd0/tmp/ld_shutdown.tmp",
                     "/dev_hdd0/tmp/ld_hooks_shutdown.txt");
    }
```

---

## 8. Phase 6: NID Scanner Memory Safety

### File: `sprx-plugin/usb_hooks.c` — `g_scan_regions`

Current regions scan from 0x00100000 up to 0x40000000, covering large swaths of potentially unmapped memory. A false-positive NID match pointing to an unmapped GOT slot would DSI.

**Corrected approach:** Instead of broad scanning, either:
1. **Constrain to known .text regions** — use the game's ELF segment boundaries
2. **Or validate addresses before dereferencing** with a safe-probe pattern

For now, the scan regions are already somewhat constrained (gaps between 0x00800000 and 0x30000000 are skipped), and the PLT stub rejection (`func_addr < 0x30000000`) catches the most common false positives.

**Additional safety addition to `scan_for_nid()`:**

```c
static int scan_for_nid(void *start, uint32_t size, uint32_t target_nid,
                         void **out_addr)
{
    uint32_t *words = (uint32_t*)start;
    uint32_t nwords = size / sizeof(uint32_t);
    uint32_t i;

    for (i = 0; i <= nwords - 3; i += 3) {
        uint32_t nid      = words[i + 0];
        uint32_t reserved = words[i + 1];
        uint32_t got_ptr  = words[i + 2];

        if (nid != target_nid) continue;
        if (reserved != 0 && reserved > 0x1000) continue;
        if (got_ptr == 0 || got_ptr < 0x00010000 || got_ptr > 0x4FFFFFFF) continue;

        /* Validate that got_ptr is at least word-aligned */
        if (got_ptr & 0x3) continue;

        /* SAFETY: Attempt to dereference GOT slot.
         * On CellOS, this will DSI if the page is unmapped.
         * Future: use sys_mmapper_get_page_attribute to validate
         * before dereference, if available from user-space. */
        uint32_t *got_slot = (uint32_t*)(uintptr_t)got_ptr;
        uint32_t func_addr = *got_slot;

        /* Reject PLT stubs (lazy binding not yet resolved) */
        if (func_addr == 0 ||
            func_addr < 0x30000000 ||
            func_addr > 0x4FFFFFFF) {
            continue;
        }

        *out_addr = (void*)(uintptr_t)func_addr;
        return 0;
    }
    return -1;
}
```

---

## 9. Phase 7: Build Deploy & Validate

### Build

```bash
cd '/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin'
make clean && make
```

### Deploy

```powershell
.\ftp-deploy.ps1
```

### Validate Sequence

| Step | Check | Method |
|------|-------|--------|
| 1 | `network_init()` succeeds | PS3 boot log: `OK: network_init(28472)` |
| 2 | `network_wait_ready()` succeeds | PS3 boot log: `OK: network_wait_ready()` |
| 3 | `usb_hook_init()` runs (VSH) | PS3 boot log: `NOTE: usb_hook_init() — VSH-only mode` |
| 4 | Enter main loop | PS3 boot log: `=== Entering main loop ===` |
| 5 | Heartbeat counting | Node.js polls `/read_process` at heartbeat_addr |
| 6 | Heartbeat at 20Hz | Should increment ~20 times per second |
| 7 | Inject into game | Test with `inject-sprx.js` |
| 8 | `ld_hooks_ready.txt` appears | Node.js polls until `STATUS=ready` |
| 9 | Game reaches "Connect Toy Pad" | No freeze during trophy sync |
| 10 | Toy Pad interactions | Full tag scan/place test |

### Expected Paper Trail

```
=== ldtoypad Full Integration: module_start ===
Creating worker thread...
OK: thread created
=== worker_thread started ===
OK: debug_init()
[NET] network_init(port=28472) ENTRY
[NET] socket() failed attempt 1/120: 0x80010010  ← sys_net not ready
[NET] socket() failed attempt 2/120: 0x80010010
...
[NET] socket()+bind() succeeded on attempt 7  ← after interfaces ready
OK: network_init(28472)
[NET] Self IP acquired via getsockname (waited 2 polls)
OK: network_wait_ready()
OK: usb_hook_init() — USB hooks installed for game  (or VSH-only)
=== Entering main loop ===
```

---

## 10. Complete Files to Modify

| File | Changes | Status |
|------|---------|--------|
| `sprx-plugin/network.c` | ENTRY log, 2s delay, 120 retry, no SO_REUSEADDR, fix last_bind_err, IPPROTO_UDP | **TO DO** |
| `sprx-plugin/main.c` | 10ms→50ms sleep, memory-mapped heartbeat in loop | **TO DO** |
| `sprx-plugin/usb_hooks.c` | heartbeat ptr in alloc_trampolines, atomic .tmp→rename for IPC | **TO DO** |
| `sprx-plugin/usb_hooks.h` | Add `volatile uint32_t *heartbeat` field to state struct | **TO DO** |
| `sprx-plugin/main.c` verify | module_stop closes socket + sys_net_finalize cleanly | **TO DO** |
| `ld-toypad-server/scripts/inject-sprx.js` | `unload_prx` before `load_prx` | **TO DO** |
| `pull-papertrail.ps1` | Add heartbeat_addr read via PS3MAPI | **NICE TO HAVE** |
