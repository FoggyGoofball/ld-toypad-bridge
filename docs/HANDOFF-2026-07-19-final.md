# HANDOFF REPORT: LD-ToyPad Bridge SPRX — State as of 7/19/2026 10:56 AM

## EXECUTIVE SUMMARY

We are porting a LEGO Dimensions Toy Pad emulator from **PSL1GHT SDK** to the **Sony DUPLEX SDK (3.40)**. The SPRX plugin runs on a PS3 (Cobra CFW 8.20+, webMAN) and bridges the physical Toy Pad USB HID device to a PC server via UDP.

**PRIORITY STATUS: Complete PSL1GHT → Sony SDK source port is DONE for all source files.** The last build achieved:

- ✅ **module_start/stop** with thread delegation (Sony SDK `-mprx`)
- ✅ **cellFsOpen/Write/Close** for papertrail logging to boot log
- ✅ **socket() retry loop** — confirmed **"OK: socket() succeeded, network is up!"** in boot log
- ✅ All source files (`main.c`, `network.c`, `debug.c`, `debug.h`, `ldd_driver.c`, `ldd_driver.h`, `toypad_state.c`, `toypad_state.h`, `compat.c`) are **already Sony SDK clean** (BSD sockets, `<cell/cell_fs.h>`, `<cell/usbd.h>`, `sys_timer_usleep()`)
- ✅ PS3 IP: `192.168.0.47`, credentials: `mike/mike`, FTP server running

## WHAT REMAINS (Needs Fresh Agent Thread)

### 1. Build Integration — Makefile needs C_SRCS update
**File:** `sprx-plugin/Makefile`
- Currently `C_SRCS := main.c compat.c` — only compiles 2 files
- **MUST change to:** `C_SRCS := main.c compat.c network.c debug.c ldd_driver.c toypad_state.c`
- Also `LDFLAGS` must include `-lusbd_stub` (for `cellUsbdInit`/`cellUsbdRegisterExtraLdd`)
- Also `-lsysmodule_stub` is currently in LDFLAGS but **NO source file uses cellSysmoduleLoadModule anymore** (it was removed because it hangs). May be harmless but should be removed for cleanliness.

**Desired LDFLAGS:**
```
LDFLAGS := -mprx -nodefaultlibs -llv2_stub -lfs_stub -lnet_stub -lusbd_stub
```

### 2. main.c — Needs full integration call chain
**File:** `sprx-plugin/main.c`
- Currently a simple `socket()` retry loop — proof-of-concept only
- **MUST call** `debug_init()`, `network_init()`, `ldd_driver_init()`, `toypad_state_init()` in the worker thread
- **MUST call** corresponding shutdowns in `module_stop`
- **MUST add `#include "debug.h"`, `#include "network.h"`, `#include "ldd_driver.h"`, `#include "toypad_state.h"`**

### 3. network.c — Has 1 remaining raw syscall (NO_SONY_SDK_API)
**File:** `sprx-plugin/network.c`
- Line 232: `sys_get_timebase_usec()` — this comes from `syscall.h` (inline asm `mftb`)
- Sony SDK provides `<sys/sys_time.h>` with `sys_time_get_system_time()` which returns time in microseconds — **BUT IT'S A DIFFERENT API** (returns 64-bit usec since epoch, not timebase ticks / 80)
- **The expert must answer:** Does `sys_time_get_system_time()` work in a PRX without `-lsysutil_stub`? If not, is the `mftb` inline asm safe to keep?

### 4. syscall.h — Questionable file
**File:** `sprx-plugin/syscall.h`
- Provides `sys_usleep()` (raw syscall 74) and `sys_get_timebase_usec()` (mftb)
- `sys_usleep()` is **NOT USED anywhere anymore** — `network.c` was updated to use `sys_timer_usleep()` from `<sys/timer.h>`
- `sys_get_timebase_usec()` is still used in `network_maybe_probe_server()`
- **Decision needed:** Keep `syscall.h` for the timebase function only, OR refactor to use Sony SDK time API

### 5. compat.c — May conflict with -lc_stub
**File:** `sprx-plugin/compat.c`
- Custom implementations of `memcpy`, `memset`, `strlen`
- Using `-nodefaultlibs` flag, so these shouldn't conflict with libc
- **BUT** if we introduce `-lc_stub`, they WILL conflict
- **Decision needed:** Do we need `-lc_stub` for anything? If yes, remove `compat.c`. If no, keep as-is.

### 6. ldd_driver.c — Uses UNVERIFIED Sony SDK API
**File:** `sprx-plugin/ldd_driver.c`
- Calls `cellUsbdInit()` and `cellUsbdRegisterExtraLdd()` from `<cell/usbd.h>`
- These are **best-guess ported** from PSL1GHT `sysUsbdInitialize()` / `sysUsbdRegisterExtraLdd()`
- **The expert must confirm** the Sony SDK `CellUsbdLddOps` structure signature (`.name`, `.probe`, `.attach`, `.detach` fields)
- The function signature `cellUsbdRegisterExtraLdd(&ops, vid, pid)` is an **educated guess** — the actual Sony SDK may have a different parameter order or different struct members
- Without a real Toy Pad to test, this code path is **UNTESTABLE** — need CFW expert to validate

### 7. Build Process Requirements
- Build toolchain: **WSL bash** (Linux tools on Windows)
- Compiler: `C:/usr/local/cell/host-win32/ppu/ppu-lv2/bin/gcc.exe` via WSL `/mnt/c/...` path
- Signing: `oscetool` from `$HOME/oscetool-src/oscetool` (WSL binary)
- Make is invoked via WSL: `cd /mnt/c/Users/Admin/source/repos/dimensions\ plugin/sprx-plugin && make clean && make`
- Output: `sprx-plugin/build/ldtoypad.sprx` (~6KB currently, will grow with all modules)

### 8. Deployment Process
- **Script:** `ftp-deploy.ps1` — uploads to PS3 via webMAN FTP
- **Trigger:** `powershell -ExecutionPolicy Bypass .\ftp-deploy.ps1`
- **Boot plugins list:** check `clean_boot_plugins.txt` for format
- **Reboot:** `curl http://192.168.0.47/restart.ps3` — wait ~90s for reboot
- **Verify:** `curl -u mike:mike "ftp://192.168.0.47/dev_hdd0/plugins/ldtoypad_boot.log"`

---

## CURRENT FILE STATE (Verified On Disk)

| File | Status | PSL1GHT Leftovers? | Notes |
|------|--------|-------------------|-------|
| `main.c` | ✅ Sony SDK | None | Need integration calls |
| `network.c` | ⚠️ 1 leftover | `sys_get_timebase_usec()` via syscall.h | Need expert ruling |
| `network.h` | ✅ Sony SDK | None | BSD sockaddr_in, fine |
| `debug.c` | ✅ Sony SDK | None | cellFsWrite + BSD sockets |
| `debug.h` | ✅ Sony SDK | None | `<stdint.h>`, `<netinet/in.h>` |
| `ldd_driver.c` | ✅ Sony SDK | None | Untested cellUsbd API calls |
| `ldd_driver.h` | ✅ Sony SDK | None | `<stdint.h>` |
| `toypad_state.c` | ✅ Sony SDK | None | No USB includes needed |
| `toypad_state.h` | ✅ Sony SDK | None | `<stdint.h>` |
| `compat.c` | ✅ Sony SDK | None | memcpy/memset/strlen |
| `syscall.h` | ⚠️ Legacy | 2 inline asm routines | `sys_usleep` unused, `sys_get_timebase_usec` still needed |
| `Makefile` | ❌ Outdated | PSL1GHT pattern | Only compiles 2 files, needs all 6 |

---

## VERIFIED BOOT LOG (Latest, Step 2 fixed SPRX)
```
=== ldtoypad Step 2: module_start ===
Creating worker thread...
OK: thread created
=== worker_thread started ===
Step 2: Retrying socket() for network...
Waiting 2s for network interface...
OK: socket() succeeded, network is up!
=== worker_thread network ready ===
=== worker_thread EXIT ===
```

**Proves:** `socket()` with `-mprx -lnet_stub` works from worker thread. Network is available ~2s after thread start. No cellSysmoduleLoadModule needed.

---

## CLARIFYING QUESTIONS FOR THE EXPERT

Please answer these and include the answers in the handoff to the new agent:

1. **`sys_time_get_system_time()` in PRX context:** Does `sys_time_get_system_time()` from `<sys/sys_time.h>` work inside a PRX without `-lsysutil_stub`? The function is declared in `<sys/sys_time.h>` (part of LV2, not sysutil), but the symbol `sys_time_get_system_time` may need a stub library. If it DOES work, we can replace the `mftb` inline asm in `network.c:232` with a clean Sony SDK call. If NOT, is the `mftb` inline asm (`sys_get_timebase_usec()`) safe to keep in a Sony SDK PRX?

2. **`cellUsbdRegisterExtraLdd()` — Sony SDK signature:** What is the EXACT signature of `cellUsbdRegisterExtraLdd` in Sony SDK 3.40? The PSL1GHT version takes (ops, vid, pid) but the Sony SDK may differ. Also, what is the exact layout of `CellUsbdLddOps`? We guessed `.name`, `.probe`, `.attach`, `.detach` — is that correct?

3. **`cellUsbdInit()` return values:** What does `cellUsbdInit()` return on success? We assumed `CELL_OK` (0) — is this correct for the Sony SDK version? And does `cellUsbdRegisterExtraLdd` return `CELL_USBD_PROBE_SUCCEEDED` on success?

4. **`-lc_stub` library:** Is `-lc_stub` available in the Sony SDK toolchain at `C:/usr/local/cell/host-win32/ppu/ppu-lv2/lib/`? If yes, linking with `-lc_stub` would provide standard `memcpy`, `memset`, `strlen` and eliminate the need for `compat.c`. If `-lc_stub` is NOT available, we must keep `compat.c` with our custom implementations.

5. **`-lusbd_stub` library:** Is `-lusbd_stub` the correct library name for the USB driver stubs in Sony SDK? Or is it `-lusb_stub` or something else? What is the exact filename at `C:/usr/local/cell/target/ppu/lib/libusbd_stub.a`?

6. **Thread safety of `debug_printf` → `cellFsWrite`:** Is the current pattern of `debug_printf` → `cellFsOpen`/`cellFsWrite`/`cellFsClose` safe to call from the worker thread (which is NOT the main PPU thread)? Some cellFs calls may require being on the "root" thread in certain CFW versions.

7. **`module_start` thread vs VSH:** When Cobra loads our PRX, what thread context does `module_start` run in? Is it VSH's event handler thread? Is `sys_ppu_thread_create` safe to call from that context? (Our current code works, confirming yes, but we want to be sure.)

8. **Extra LDD on Cobra CFW 8.20+:** Does `cellUsbdRegisterExtraLdd()` actually work on Cobra CFW 8.20+, or does it return an error? We earlier saw it "not supported" on some CFWs. If it fails, the plugin should gracefully fall back to network-only mode (which it already does in the code).

---

## NEW AGENT STARTING POINT

### Step-by-step for the fresh agent:

1. **Read this handoff document** (`docs/HANDOFF-2026-07-19-final.md`)
2. **Read the expert answers** (provided alongside this handoff)
3. **Read all files in `sprx-plugin/`** to confirm current state
4. **Update `sprx-plugin/Makefile`:**
   - Add `network.c`, `debug.c`, `ldd_driver.c`, `toypad_state.c` to `C_SRCS`
   - Add `-lusbd_stub` to `LDFLAGS`
   - Remove `-lsysmodule_stub` (no longer used)
5. **Update `sprx-plugin/main.c`:**
   - Add includes for debug, network, ldd_driver, toypad_state headers
   - Replace the simple `socket()` retry loop with the full initialization chain:
     ```c
     debug_init();
     network_init(28472);
     network_wait_ready();
     ldd_driver_init();  // may fail gracefully
     toypad_state_init();
     ```
   - Add main loop: `recvfrom` in non-blocking mode, poll for server, poll for USB device
6. **Address expert answers** (e.g., replace `sys_get_timebase_usec()` if `sys_time_get_system_time()` works)
7. **Build:** `cd /mnt/c/Users/Admin/source/repos/dimensions\ plugin/sprx-plugin && make clean && make`
8. **Verify ELF:** `powerpc64-ps3-elf-objdump -x build/ldtoypad.prx` (or via WSL) — check sections
9. **Deploy:** `powershell -ExecutionPolicy Bypass .\ftp-deploy.ps1`
10. **Reboot PS3 and check boot log**
11. **Test bridge server:** `cd ld-toypad-server && node server.js`
