# Expert Diagnostic Report — 2026-07-21
## LD-ToyPad Bridge: inject-sprx.js Game Detection Failure + SPRX network_init Regression

---

## Summary

**Two independent blocking issues prevent the injection pipeline from completing:**

**Issue A** (NEW, injector-side): The `inject-sprx.js` script now correctly receives JSON from webMAN MOD 1.47.48c+ (`{"response": "0x0000000001010200"}`) but was parsing the wrong JSON key (`json.result` instead of `json.response`).

**Issue B** (KNOWN, SPRX-side): Even after detection succeeds, the SPRX itself crashes at `network_init()` before any hook code is reached.

---

## Issue A: PID Detection — JSON Key Mismatch

### Evidence

The injector polls `GET /ps3mapi.ps3?PROCESS%20GETCURRENTPID` and receives:

```json
{"response": "0x0000000001010200"}
```

The PID `0x1010200` = 16843008 is a valid game process ID (the game IS booted). However, the parser was looking for `json.result` (the key the handoff document assumed), but webMAN MOD 1.47.48c+ returns `json.response`.

### Fix Applied (2026-07-21, commit pending)

```javascript
// Updated parseJsonPid() — now checks both keys:
const pidStr = (json && json.response) ? String(json.response) :
               (json && json.result) ? String(json.result) : null;
```

### Remaining Question for Expert

> **Q-EXPERT-A1:** Is the `"response"` key consistent across all webMAN MOD 1.47.48c+ JSON endpoints (`PROCESS GETCURRENTPID`, `MODULE LOAD`, `MODULE UNLOAD`, `MEMORY SET`)? Or does each endpoint use a different key? Specifically:
> - `PROCESS GETCURRENTPID` → `{"response": "0x..."}` (confirmed)
> - `MODULE LOAD` → `{"response": "success"}` or `{"result": "success"}`?
> - `MODULE UNLOAD` → `{"response": "success"}` or `{"result": "success"}`?
> - `MEMORY SET` → `{"response": "ok"}` or `{"result": "ok"}`?

---

## Issue B: SPRX network_init() Regression

### Current Status: UNCHANGED

Per the papertrail analysis (2026-07-20), the SPRX still terminates at `network_init()`:

```
=== ldtoypad Full Integration: module_start ===
Creating worker thread...
OK: thread created
=== worker_thread started ===
OK: debug_init()
FAIL: network_init() — fatal, cannot continue
```

The `ld_hooks_ready.txt` IPC file is **never written** because `usb_hook_init()` is never called. All 5 hooking defects sit untested behind this blocker.

### Root Cause (Expert-Confirmed)

The orphaned worker thread from the **previous** SPRX injection still holds `g_net.socket_fd` open (the `module_stop` was never called between injections). Since UDP has no TIME_WAIT (connectionless), the port is held by the **live thread**, not by the kernel. The retry loop exhausts its 30×100ms attempts and returns -1.

### Current Fixes Applied to Source Code

All of the following were approved in the 2026-07-21 expert review and are already in the source files:

| Fix | File | Detail |
|-----|------|--------|
| ✅ ENTRY debug log | `network.c:49` | `DEBUG_PRINT("[NET] network_init(port=%u) ENTRY\n")` |
| ✅ 2s stabilization delay | `network.c:65` | `sys_timer_usleep(2000000)` before retry loop |
| ✅ 30→120 retries | `network.c:86` | 12 seconds total at 100ms intervals |
| ✅ `last_bind_err` fix | `network.c:108` | Stores `bind_ret` result, not `g_net.socket_fd` |
| ✅ IPPROTO_UDP | `network.c:88` | Explicit `IPPROTO_UDP` (17) in socket() |
| ✅ `IPPROTO_UDP` symbol | `compat.c` | `#define IPPROTO_UDP 17` if undefined by SDK |
| ✅ Explicit `module_stop` unload | `main.c:83-104` | Closes socket, finalizes sys_net, joins thread |
| ✅ Node.js pre-unload | `inject-sprx.js` | `MODULE UNLOAD` before `MODULE LOAD` |
| ✅ 50ms sleep (20Hz) | `main.c:235` | Changed from 10ms to 50ms |

### Question for Expert

> **Q-EXPERT-B1:** Despite applying all the above fixes, the `network_init()` may still fail because the **previous SPRX process is never terminated**. The Node.js orchestrator does `MODULE UNLOAD` then `MODULE LOAD`, but `MODULE UNLOAD` sends a signal to the PRX which triggers `module_stop()` on the worker thread. **What is the actual timing behavior?**
>
> - Does `MODULE UNLOAD` wait for `module_stop()` to complete (i.e., the thread joins, socket closes) before returning the HTTP response?
> - Or does it return immediately and the cleanup runs asynchronously?
> - If asynchronous: how long should we wait before `MODULE LOAD` to ensure the port is released? Our current wait is 500ms.

> **Q-EXPERT-B2:** The PS3 returns `{"response": "0x0000000001010200"}` which appears to be a 64-bit hex value with leading zeros (`0x0000000001010200`). Is this the actual PID format for webMAN MOD 1.47.48c+, or should we strip leading zeros before parsing? Our parser handles this correctly via `parseInt(hexMatch[1], 16)` which ignores leading zeros, but we want to confirm this is the expected format.

> **Q-EXPERT-B3:** We observe intermittent `socket hang up` errors and `timeout` errors when polling `/ps3mapi.ps3?PROCESS%20GETCURRENTPID`. These occur roughly once every 30-40 seconds. Is this normal behavior for webMAN MOD (e.g., the HTTP server thread yields periodically), or does it indicate a network stability issue?

---

## Complete Failure Timeline (Most Recent Test)

```
User: boots LEGO Dimensions to "Connect Toy Pad" screen
User: launches start-bridge-and-inject.bat
  → Step 1: webMAN detected OK  (✓)
  → Step 2: Polls /ps3mapi.ps3?PROCESS%20GETCURRENTPID
    → Receives: {"response": "0x0000000001010200"}
    → PARSER FAILED: looked for json.result, got json.response
    → Kept polling until timeout (30-60 seconds)
    → NO injection ever attempted
  → SPRX never loaded
  → Game continues normally (no crash, no hooks)
```

**After the parser fix**, the expected flow would be:
```
  → Step 2: Parses PID=0x1010200 (16843008) ✓
  → Step 3: Wait 30s for stabilization
  → Step 4: MODULE UNLOAD old PRX → MODULE LOAD new SPRX
  → Step 5: Poll for ld_hooks_ready.txt
  → Step 6: MEMORY SET preambles
```

---

## Action Required

Please confirm:
1. **Issue A** is resolved with the `json.response` parser fix (one test needed)
2. **Issue B** requires testing the SPRX with the new `network_init()` changes (120 retries, 2s delay) to see if the retry exhaustion is resolved
3. Answer the questions above to resolve any remaining uncertainty about webMAN MOD API behavior
