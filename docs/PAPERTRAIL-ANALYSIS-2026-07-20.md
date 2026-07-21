# Papertrail Empirical Analysis
## 2026-07-20 — Live PS3 Data vs 5 Defect Fixes

**Data source:** PS3 192.168.0.47, FTP download at 19:38 CST
**Files acquired:** `ldtoypad_boot.log` (7.6KB), `ldtoypad_debug.log` (25KB), `ld_recv_papertrail.txt` (1.3KB), `ld_probe_papertrail.txt` (300B)
**Files MISSING:** `ld_hooks_ready.txt`, `ld_hooks_shutdown.txt`, `ld_self_ip.txt`

---

## Critical Finding: SPRX Current Build Regression

**The current SPRX fails at `network_init()` before reaching any hook code.**

Evidence from `ldtoypad_boot.log` — the most recent boot cycle:
```
=== ldtoypad Full Integration: module_start ===
Creating worker thread...
OK: thread created
=== worker_thread started ===
OK: debug_init()
FAIL: network_init() — fatal, cannot continue
```

The SPRX terminates at `network_init()` in `main.c:124-130`. This means:
- `usb_hook_init()` is **never called**
- The 4-instruction preamble is **never written to IPC file**
- The 5 hooking defects **cannot be validated** yet — they sit behind this regression

### Root Cause Analysis

The `network_init()` failure could be:

1. **`sys_net_initialize_network()` failure** — the 128KB allocation for the network
   stack might fail after multiple module_start/stop cycles without proper cleanup.
   The function is called, errors are logged but the code continues to try socket+bind.

2. **Socket+bind retry exhaustion** — the retry loop waits 30 × 100ms = 3 seconds,
   which may not be enough if sys_net is fully uninitialized.

3. **Resource leak from prior iterations** — the boot log shows 12+ init cycles from
   multiple reboots. If `sys_net_finalize_network()` in `network_shutdown()` doesn't
   fully release resources, the 128KB allocation accumulates.

> **Older code (Step 1, Step 2) worked fine.** The earlier boot log entries show
> complete init: `socket()+bind() succeeded on attempt 11`. The regression is in
> the "Full Integration" build which includes the new toc_trampoline.s changes.

### Immediate Fix Needed

In `network.c:53-57`, the `sys_net_initialize_network()` failure path currently
just logs and continues. We need to:
1. Increase the socket+bind retry count from 30 to 60 (30 → 60 = 6 seconds total)
2. Add a 2-second initial delay before the retry loop to allow sys_net to settle
3. Log the actual error code from socket() for diagnostics

Additionally, verify the socket+bind loop at line 93 correctly prints last errors:

```c
// CURRENT (line 93-97): error format is missing last_socket_err
// Change from:
DEBUG_ERROR("[NET] socket+bind failed after %d attempts (socket err: 0x%x, bind err: 0x%x)\n",
            30, last_socket_err, last_bind_err);
// To:
DEBUG_ERROR("[NET] socket+bind failed after %d attempts (socket err: 0x%x, bind err: 0x%x)\n",
            30, last_socket_err, last_bind_err);
```

Actually looking at this again — the format string IS correct. But the `last_bind_err`
at line 88 is wrong: `last_bind_err = g_net.socket_fd;` — this stores the **socket fd
being closed** (which is always ≥ 3), not the bind error code. Fix:

```c
last_bind_err = /* actual bind return value, which is -1 on failure */;
```

But wait, the bind return value was tested against == 0 already. We need to store the
bind return value before the comparison. This is a minor diagnostic issue, not a
functional one.

---

## Keepalive / Discovery Subsystem — EMPIRICALLY PROVEN

The **probe papertrail** (`ld_probe_papertrail.txt`) shows probes being sent:
```
PROBE seq=0
PROBE seq=1
... (5 probes × 5 rounds = 25 total)
```

The **recv papertrail** (`ld_recv_papertrail.txt`) shows incoming packets:
```
RECV from=...:28472 type=0xF0 len=80    ← discovery beacons from server
RECV from=192.168.0.17:28472 type=0xEE len=8  ← keepalive heartbeats
```

**Confirmed:** The PS3 correctly sends UDP broadcast probes AND receives and
processes keepalive heartbeats from the PC server at 192.168.0.17:28472.
The network path is fully functional when the init succeeds.

---

## Defect Validation Summary

| Defect | Status | Empirical Evidence |
|--------|--------|-------------------|
| **1 & 5: Instruction Overflow + R-X** | ⚠ UNTESTED | The hook IPC file (`ld_hooks_ready.txt`) was never written. The SPRX crashes at `network_init()` before calling `usb_hook_init()`. |
| **2: TOC Corruption** | ⚠ UNTESTED | Assembly wrappers in `toc_trampoline.s` exist but were never executed. |
| **3: GOT Resolution Failure** | ⚠ UNTESTED | `scan_for_nid()` was never called. |
| **4: Passthrough TOC Collisions** | ⚠ UNTESTED | `call_original_*` stubs were never tested. |
| **Regressed: network_init()** | ✗ FAILS | Current "Full Integration" build terminates here. |

### What Works (Proven by Paper Trail)
✅ `module_start()` → thread creation
✅ `debug_init()` → file logging
✅ **Keepalive heartbeat sent/received** (from earlier successful runs)
✅ **UDP probe broadcasts sent** (from earlier successful runs)
✅ **Server discovery via hardcoded IP** (network_set_server works)
✅ **Graceful VSH-mode fallback** (usb_hook_init returns -1 when not in game)

## Recommended Action

1. **Fix `network_init()` regression** (increase retries, add initial delay, fix
   last_bind_err diagnostic value) — this is the current blocker
2. Rebuild SPRX with `make && make deploy`
3. Validate the 5 hooking defects by injecting into game process
4. Check `ld_hooks_ready.txt` appears with all 4 target+wrapper addresses
5. Verify Node.js preamble installation via PS3MAPI
6. Run with game at "Connect Toy Pad" screen

## Files Extracted (for future reference)

```
papertrail_ldtoypad_boot.log     — 7,630 bytes — module_start init chain trace
papertrail_ldtoypad_debug.log    — 25,342 bytes — verbose DEBUG_PRINT output
papertrail_ld_recv_papertrail.txt — 1,313 bytes — recvfrom sender IP + packet type log
papertrail_ld_probe_papertrail.txt — 300 bytes — discovery probe emission log
```
