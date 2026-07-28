# Expert Consultation — 2026-07-28: Finding libusbd.sprx Runtime Base (RESOLVED)

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.4), webMAN MOD 1.47.48q
**Game:** LEGO Dimensions BLUS31473, PID 0x1010200
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2`
**SPRX:** ldtoypad.sprx (22,608 bytes), injected via PS3MAPI MODULE LOAD at T+60s
**Status:** ❌ `sys_prx_get_module_list` also causes hard freeze — all LV2 module enumeration blocked from injected PRX context

---

## Problem Statement

We need the **runtime base address** of `libusbd.sprx` (CellOS USB driver) within the
game process to compute absolute addresses for 5 `cellUsbd*` hook targets:

```
target = libusbd_base + known_offset
```

Offsets have been extracted offline from the decrypted firmware ELF via OPD scanning.
The base address is the **sole remaining blocker** — 15 approaches tried, all failed.

---

## What We've Exhausted

### PRX-Side Syscall Approaches (ALL DEAD)

| # | Approach | Result | Details |
|---|----------|--------|---------|
| 1 | `sys_prx_get_module_id_by_name("cellUsbd")` | ❌ Returns -1 | Also tried `"libusbd"` |
| 2 | `sys_prx_get_module_id_by_name("libusbd")` | ❌ Returns -1 | Same failure |
| 3 | Brute-force `sys_prx_get_module_info(id)` IDs 1-255 | ❌ **HARD FREEZE** | Required power-button shutdown. Kernel panic from invalid module IDs |
| 4 | `sys_prx_get_module_id_by_address(0x02C00100...)` 6 probes | ❌ Returns -1 | All 6 probe addresses across 0x02C0-0x02E0 range returned -1 |

**Conclusion:** All syscall-based module enumeration is blocked from PS3MAPI-injected
PRX context. The lv2 kernel restricts these calls when running in a game process.

### PS3MAPI-Side Approaches

| # | Approach | Result | Details |
|---|----------|--------|---------|
| 5 | PS3MAPI `MEMORY GET` scan 0x02000000-0x10000000 | ❌ libusbd not found | 64MB-step scan. Only non-zero regions at 0x02000000, 0x02800000, 0x10000000 |
| 6 | Search for libusbd `.text` signature (`386000004e800020`) | ❌ Not in any mapped region | First 8 bytes of libusbd .text: `li r3,0; blr` |
| 7 | Search for ELF magic (`7F454C46`) at candidate bases | ❌ Not in runtime memory | ELF header stripped at load time; only LOAD segments mapped |

**Conclusion:** libusbd is NOT mapped in the game process's user-accessible address
space. It likely resides in a shared kernel region inaccessible to PS3MAPI MEMORY GET.

### OPD / GOT Resolver Approaches

| # | Approach | Result | Details |
|---|----------|--------|---------|
| 8 | OPD resolver → stub → GOT → read function pointer | ⚠️ All resolve to 0x2690384 | Shared lazy-binding resolver stub, not real libusbd addresses |
| 9 | Ping cellUsbd* with realistic args to trigger lazy binding | ❌ GOT unchanged | SDK wrapper validates args & returns early, never calls through GOT |

**Debug data from approach #8:**
```
SPRX TOC = 0x2698340
OpenPipe:        stub=0x268044C  lwz r9,-0x8000(r2)  got=0x2690340 → real=0x2690384
InterruptTransfer: stub=0x2680734  lwz r9,-0x8000(r2)  got=0x2690340 → real=0x2690384
ClosePipe:       stub=0x26803EC  lwz r9,-0x8000(r2)  got=0x2690340 → real=0x2690384
```
All three share the same GOT slot (TOC-0x8000) and same resolver stub. The SDK
wrapper functions (`lwz r9, offset(r2)` → `mflr r0` → `stdu r1, -0x70(r1)` → ...)
validate arguments before calling through the GOT-loaded pointer. Even with
stack-allocated realistic arguments, the wrappers return early without triggering
lazy binding.

### Offline Binary Analysis

| # | Approach | Result | Details |
|---|----------|--------|---------|
| 10 | scetool 0.9.2 decrypt libusbd_493.sprx → ELF | ✅ 47,296 byte ELF | Used existing keys up to v481; SELF used older key revision |
| 11 | `nm` / symbol table extraction | ❌ Stripped, no symbols | Production firmware module |
| 12 | NID search (0x7F5F00D3 etc.) in ELF | ❌ Not found | Export FNIDs differ from import NIDs |
| 13 | OPD scanning → code address mapping | ✅ 38 OPDs, 82 functions | Identified 11 cellUsbd API exports |
| 14 | Manual export table parsing | ⚠️ Found structure | Mixed-endian fields complicate parsing |
| 15 | Ghidra 12.1.2 headless analysis | ⚠️ Analysis OK, no symbols | Stripped binary produces auto-named functions only |

### What Works

- ✅ SPRX init chain: LDD → network → debug → toypad_state all succeed
- ✅ OPD fallback hooks activate when usb_hook_init fails
- ✅ 5 function offsets extracted via OPD scanning:
  ```
  #define LIBUSBD_OFFSET_OPENPIPE       0x00000244
  #define LIBUSBD_OFFSET_INTERRUPT_XFER 0x000004B4
  #define LIBUSBD_OFFSET_CLOSEPIPE      0x00000380
  #define LIBUSBD_OFFSET_GET_DEV_DESC   0x0000061C
  #define LIBUSBD_OFFSET_CONTROL_XFER   0x000007C8
  ```

---

## EXPERT RESPONSE (2026-07-28)

The expert confirmed:

1. **The lazy-binding approach would instantly crash** — the SDK wrappers point to
   thick wrappers, not raw PLT stubs. The `bl` scanning trick would lose the
   resolver's register context (`r11`, `r12`, LR) and panic the kernel.

2. **The `-0x8000(r2)` offset** is not a function-specific GOT slot — it's the
   shared scelibstub resolver entry point at the top of the TOC/GOT block.

3. **`sys_prx_get_module_list` is the golden API** we labeled "NOT YET TESTED."
   It safely returns only valid, loaded module IDs from the kernel. No crash risk.

4. **The module name is `"cellUsbd_Library"`** — not `"libusbd"` or `"cellUsbd"`,
   which is why `sys_prx_get_module_id_by_name` failed. Substring matching
   (`"Usbd"` or `"usbd"`) catches all variants.

## IMPLEMENTED SOLUTION (2026-07-28)

The entire lazy-binding + assembly-parsing code (~120 lines) was deleted and
replaced with this clean, safe implementation:

```c
#include <sys/prx.h>

/* Inline strstr — not available with -nodefaultlibs */
static int contains(const char *haystack, const char *needle) {
    const char *h, *n;
    for (; *haystack; haystack++) {
        for (h = haystack, n = needle; *n && *h == *n; h++, n++);
        if (!*n) return 1;
    }
    return 0;
}

int usb_hook_init(void)
{
    int ret;

    if (g_usb_hooks.initialized) return 0;
    memset(&g_usb_hooks, 0, sizeof(g_usb_hooks));
    g_usb_hooks.next_pipe_id = 0x1000;

    /* Step 1: Find libusbd.sprx base via sys_prx_get_module_list.
     * This is the ONLY safe API from injected PRX context — it returns
     * only valid, currently-loaded module IDs from the kernel. */

    sys_prx_id_t id_list[128];
    sys_prx_get_module_list_t list_info;
    list_info.size  = sizeof(list_info);
    list_info.max   = 128;
    list_info.count = 0;
    list_info.idlist = id_list;

    if (sys_prx_get_module_list(0, &list_info) != CELL_OK) {
        papertrail("[USB] FATAL: sys_prx_get_module_list failed");
        DEBUG_ERROR("[USB] sys_prx_get_module_list returned error\n");
        return -1;
    }

    DEBUG_PRINT("[USB] Module list: %u loaded modules\n",
                (unsigned)list_info.count);

    for (size_t i = 0; i < list_info.count; i++) {
        sys_prx_module_info_t minfo;
        minfo.size = sizeof(minfo);

        if (sys_prx_get_module_info(id_list[i], 0, &minfo) == CELL_OK) {
            /* Substring match: catches "cellUsbd", "cellUsbd_Library", "libusbd" */
            if (contains(minfo.name, "Usbd") || contains(minfo.name, "usbd")) {
                g_libusbd_base = (uint32_t)(uintptr_t)minfo.segments[0].base;

                papertrail("[USB] libusbd base found via module list");
                DEBUG_PRINT("[USB] Found '%s' id=%d base=0x%08X segments=%u\n",
                            (const char*)minfo.name, (int)id_list[i],
                            (unsigned)g_libusbd_base,
                            (unsigned)minfo.segments_num);

                /* Format base into temp buffer for papertrail */
                {   char buf[64];
                    int j = 0;
                    const char *s = "[USB] libusbd base="; while (*s) buf[j++]=*s++;
                    uint32_t v = g_libusbd_base;
                    buf[j++]='0'; buf[j++]='x';
                    for (int sh=28; sh>=0; sh-=4) { int n=(v>>sh)&0xF; buf[j++]=n<10?'0'+n:'A'+n-10; }
                    buf[j]=0; papertrail(buf);
                }

                goto found_libusbd;
            }
        }
    }

    papertrail("[USB] FATAL: libusbd not found in module list");
    DEBUG_ERROR("[USB] Scanned %u modules, none matched 'Usbd'/'usbd'\n",
                (unsigned)list_info.count);
    return -1;

found_libusbd:
    {
    /* Step 2: Compute absolute addresses from base + offsets */
    uint32_t target_openpipe   = g_libusbd_base + LIBUSBD_OFFSET_OPENPIPE;
    uint32_t target_transfer   = g_libusbd_base + LIBUSBD_OFFSET_INTERRUPT_XFER;
    uint32_t target_closepipe  = g_libusbd_base + LIBUSBD_OFFSET_CLOSEPIPE;
    uint32_t target_getdevdesc = g_libusbd_base + LIBUSBD_OFFSET_GET_DEV_DESC;
    uint32_t target_ctrlxfer   = g_libusbd_base + LIBUSBD_OFFSET_CONTROL_XFER;

    /* Store for passthrough (target + 16 = skip preamble) */
    g_real_openpipe_addr  = target_openpipe;
    g_real_transfer_addr  = target_transfer;
    g_real_closepipe_addr = target_closepipe;

    /* Step 3: Allocate executable trampoline page and install hooks */
    if (install_hooks() != 0) {
        DEBUG_ERROR("[USB] Hook installation failed\n");
        return -1;
    }

    /* Step 4: Write IPC file to PLUGINS directory */
    write_ipc_file(target_openpipe, target_transfer, target_closepipe,
                    target_getdevdesc, target_ctrlxfer);

    DEBUG_PRINT("[USB] Static-offset hooks: 5 targets from libusbd base 0x%08X\n",
                (unsigned)g_libusbd_base);

    g_usb_hooks.initialized = 1;
    return 0;
    }
}
```

### Why this works (vs all 15 failed approaches)

| Previous approach | Why it failed | Why this succeeds |
|-------------------|---------------|-------------------|
| `get_module_id_by_name("libusbd")` | Actual name is `"cellUsbd_Library"` | Substring match catches all variants |
| Brute-force `get_module_info(1..255)` | Invalid IDs panic kernel | Only queries valid IDs from kernel |
| `get_module_id_by_address` | Blocked from PRX context | Unnecessary — using the list API |
| PS3MAPI memory scanning | libusbd in kernel space | Direct kernel query, no memory scan |
| Lazy-binding stub call | Would crash (lost resolver context) | Doesn't touch GOT or resolver at all |

### Build Result

```
$ make
  CC    usb_hooks.c   ✓
  LD    build/ldtoypad.prx  ✓
  SPRX  build/ldtoypad.sprx  ✓
  22608 bytes — signed, zero warnings
```

## CONSOLE TEST RESULT (2026-07-28) ❌

`sys_prx_get_module_list` was deployed (22,608 bytes) and tested at the "Connect
ToyPad" screen. **Result: HARD FREEZE** — same behavior as brute-force ID scanner.

Boot log shows init chain complete through `toypad_state_init()`, then freeze at
`usb_hook_init()` → `sys_prx_get_module_list()`.

**Conclusion: ALL LV2 syscall-based module enumeration is blocked from
PS3MAPI-injected PRX context, including the "safe" list API.** The PRX runs in a
sandbox without sufficient LV2 permissions for any module queries.

### Updated Failure Table

| # | Approach | Result |
|---|----------|--------|
| 1 | `sys_prx_get_module_id_by_name` | ❌ Returns -1 |
| 2 | Brute-force `sys_prx_get_module_info` | ❌ Hard freeze |
| 3 | `sys_prx_get_module_id_by_address` | ❌ Returns -1 |
| 4 | **`sys_prx_get_module_list`** | ❌ **Hard freeze** |

### Remaining Options (Expert Guidance Needed)

1. **Hook game's EBOOT import stubs** — The game's resolved cellUsbd imports ARE in
   user-accessible memory. Find the game's GOT/PLT entries and overwrite them
   with trampoline pointers. Module enumeration unnecessary.

2. **Cobra CFW lv2 peek/poke** — Evilnat 4.93 includes Cobra 8.4. Direct lv2
   syscalls may bypass the PRX sandbox for memory scanning.

3. **PS3MAPI MEMORY SET from Node.js** — Move preamble writing entirely to the
   Node.js side. PS3MAPI runs with VSH privileges and can write to any process
   memory. Still need addresses to write to.

4. **Extend OPD callback-stealing fallback** — The existing OPD hooks handle
   `cellUsbdRegisterLdd` and `cellUsbdInterruptTransfer`. Could be extended to
   cover GetDeviceDescriptor and ControlTransfer without needing libusbd's base.

### Awaiting Console Test

The SPRX is built and committed. Test procedure:
1. Power on PS3, launch LEGO Dimensions, reach "Connect ToyPad" screen
2. Deploy SPRX via FTP, run `start-bridge-and-inject.bat`
3. Check boot log for `[USB] libusbd base=0xXXXXXXXX`
4. If found: 5 hook targets computed, IPC file written, Node.js writes preambles

---

## Decrypted ELF Reference

```
File: libusbd_493.elf (47,296 bytes)
Arch: PPC64 big-endian, ELF64, stripped

Segments:
  LOAD RE:  vaddr 0x0000, file 0x00F0, size 0x9800  (.text, 82 functions)
  LOAD RW:  vaddr 0x9800, file 0x98F0, size 0x0380  (.data, 38 OPDs)
  LOPROC:   vaddr 0x0000, file 0x9C70, size 0x1C50  (export/stub metadata)

Module info at file 0x95D4:
  modattribute: 0x0006
  modversion:   0x0101
  modname:      "cellUsbd_Library"
  ent_top:      0x9474  (export table)
  ent_end:      0x94AC
  stub_top:     0x94B4  (stub table)
  stub_end:     0x94E0

Extracted cellUsbd offsets (from OPD scanning, verified against 82 prologues):
  cellUsbdInit:              0x0010  (entry point, no GOT load)
  cellUsbdEnd:               0x0120
  cellUsbdOpenPipe:          0x0244  ← used for base computation
  cellUsbdClosePipe:         0x0380
  cellUsbdInterruptTransfer: 0x04B4
  cellUsbdGetDeviceDescriptor: 0x061C
  cellUsbdControlTransfer:   0x07C8
  cellUsbdRegisterLdd:       0x0944
  cellUsbdUnregisterLdd:     0x0A8C
  cellUsbdScanDevice:        0x0BB4
  cellUsbdAttachLdd:         0x0D00
```

## Build & Deployment

```
SDK: DUPLEX 3.40
Compiler: ppu-lv2-gcc -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs
Link: -llv2_stub -lfs_stub -lnet_stub -lusbd_stub -lsysmodule_stub
Sign: oscetool 0.9.2 -5 APP (via WSL)
Output: 22,816 bytes
Inject: PS3MAPI MODULE LOAD at T+60s via Node.js
```
