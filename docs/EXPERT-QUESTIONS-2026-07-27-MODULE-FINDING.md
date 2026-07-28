# Expert Questions — 2026-07-27: Finding libusbd.sprx Base Address (UPDATED 18:30)

## Context

LD-ToyPad Bridge SPRX project on PS3 (Evilnat **4.93** CFW, webMAN MOD 1.47.48q).
We need to find the **runtime base address of `libusbd.sprx`** (CellOS USB driver)
within the LEGO Dimensions (BLUS31473) game process. Once we have the base,
we'll add hardcoded function offsets (extracted offline from the firmware's
libusbd ELF via Ghidra) to compute hook target addresses for 5 `cellUsbd*` functions.

After 14 days of failed approaches, **all PRX-side and PS3MAPI-side module-finding
methods have been exhausted and failed.**

## What We Tried — Updated Results

### PRX-Side Syscall Approaches (ALL DEAD)

| # | Method | Result |
|---|--------|--------|
| 1 | `sys_prx_get_module_id_by_name("cellUsbd")` / `("libusbd")` | ❌ Returns -1 |
| 2 | Brute-force `sys_prx_get_module_info(id, ...)` IDs 1-255 | ❌ **HARD FREEZE** — kernel panic |
| 3 | `sys_prx_get_module_id_by_address(0x02C00100...)` 6 probes | ❌ Returns -1 (tested on console 18:20) |

All syscall-based module enumeration is blocked from PS3MAPI-injected PRX context.
The lv2 kernel appears to restrict these calls when made from a game process.

### PS3MAPI-Side Memory Scanning

| # | Method | Result |
|---|--------|--------|
| 4 | `MEMORY GET` scan 0x02000000-0x10000000 in 64MB steps | ❌ libusbd not found at any address |
| 5 | Search for libusbd `.text` signature (`386000004e800020`) | ❌ Not found in any mapped region |
| 6 | Search for ELF magic (`7F454C46`) at candidate bases | ❌ Not present in runtime memory |

**Key finding:** libusbd does NOT appear to be mapped in the game process's
user-accessible address space. The only non-zero regions above 0x02000000
are at 0x02000000, 0x02800000, and 0x10000000 — none contain libusbd code.
libusbd may reside in a shared kernel region inaccessible to PS3MAPI `MEMORY GET`.

### Offline ELF Analysis

| # | Method | Result |
|---|--------|--------|
| 7 | scetool 0.9.2 decryption of libusbd_493.sprx | ✅ SUCCESS — 47,296 byte ELF extracted |
| 8 | `nm` / symbol table | ❌ Stripped — no symbols |
| 9 | NID search (0x7F5F00D3, etc.) in ELF | ❌ Export FNIDs differ from import NIDs |
| 10 | Manual export table parsing | ⚠️ Found `_sceModuleInfo` and export boundaries, but SCE export format complex |

### What Still Works

- ✅ SPRX init chain: LDD → network → debug → toypad_state all succeed
- ✅ OPD fallback hooks activate when usb_hook_init fails
- ✅ Decrypted libusbd ELF available for Ghidra analysis
- ✅ 82 function prologues identified in .text section
- ✅ Export table located (ent_top=0x9474, ent_end=0x94AC)
- ✅ Function address table candidates found at vaddr 0x9800+

## Decrypted ELF Details

- **File:** `libusbd_493.elf` (47,296 bytes)
- **Firmware:** 4.93 (Evilnat CFW, CECH-2501A)
- **Arch:** PPC64 big-endian, ELF64
- **Segments:**
  - LOAD RE: vaddr 0x0000, file 0xF0, size 0x9800 (.text)
  - LOAD RW: vaddr 0x9800, file 0x98F0, size 0x380 (.data)
  - LOPROC+0xa4: file 0x9C70, size 0x1C50 (export/stub metadata)
- **Module info:** `_sceModuleInfo` at file 0x95D4, modname="cellUsbd_Library"
- **Exports:** ent_top=0x9474, ent_end=0x94AC, stub_top=0x94B4, stub_end=0x94E0
- **Functions:** 82 prologues found in .text

## Planned: Ghidra Analysis

Next step is loading `libusbd_493.elf` into Ghidra (PPC64 big-endian) to:
1. Auto-analyze all function boundaries and cross-references
2. Parse the SCE PRX export table format properly
3. Extract exact offsets for: cellUsbdOpenPipe, cellUsbdInterruptTransfer,
   cellUsbdClosePipe, cellUsbdGetDeviceDescriptor, cellUsbdControlTransfer
4. Generate `#define LIBUSBD_OFFSET_*` values for usb_hooks.c

## Remaining Blocker

Even with perfect offsets from Ghidra, we still need the **runtime base address**
of libusbd in the game process. Since no PRX-side or PS3MAPI-side approach can
find it, we need an alternative strategy:

### Open Questions

1. **Is libusbd loaded in a shared kernel region?** If so, what's the virtual
   address, and can we write to it? The preamble overwrite requires write access
   to libusbd's .text page.

2. **Can we use lv2 syscalls directly?** Cobra CFW enables lv2 peek/poke.
   Can our PRX call `lv2_peek`/`lv2_poke` to scan kernel memory?

3. **Can we hook at the game level instead?** Rather than hooking libusbd
   globally, could we hook the game's resolved import stubs in the EBOOT's
   GOT/PLT? The game's cellUsbd imports ARE resolved and accessible.

4. **Can we use the OPD fallback approach for all 5 functions?** The current
   OPD fallback hooks `cellUsbdRegisterLdd` and `cellUsbdInterruptTransfer`.
   Could it be extended to cover GetDeviceDescriptor and ControlTransfer?

5. **Is there a Cobra/CFW-specific API for module enumeration?** Evilnat 4.93
   includes Cobra 8.4. Are there Cobra-specific syscalls for finding loaded
   modules?

6. **Can we write the preamble via PS3MAPI even without knowing the base?**
   If PS3MAPI's `MEMORY SET` can write to kernel/shared memory (where libusbd
   likely resides), we could write the preamble there. But we'd still need
   the base address to compute target addresses.

## Build Environment (unchanged)

- **SDK**: Sony DUPLEX SDK 3.40, `ppu-lv2-gcc`
- **Flags**: `-mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs`
- **Stub libs**: `-llv2_stub -lfs_stub -lnet_stub -lusbd_stub -lsysmodule_stub`
- **Injection**: PS3MAPI `MODULE LOAD` via Node.js at T+60s
- **PS3**: CECH-2501A, Evilnat 4.93 CFW, webMAN MOD 1.47.48q
