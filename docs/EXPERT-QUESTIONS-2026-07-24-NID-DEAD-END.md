# Expert Questions — 2026-07-24: NID Scanner Dead End + Path Forward

## Context

LD-ToyPad Bridge SPRX project. We inject a PRX into the LEGO Dimensions game
process on PS3 (Cobra CFW, webMAN MOD 1.47.48c+). The SPRX needs to find and
overwrite the game's cellUsbd GOT slots to intercept USB traffic for Toy Pad
emulation.

After 20+ build/deploy/test cycles today, we've hit a dead end with NID-based
scanning and need expert guidance on the correct next approach.

## Build Environment

- **SDK**: Sony DUPLEX SDK (SN Systems / SDK 3.40)
- **Compiler**: `C:\usr\local\cell\host-win32\ppu\ppu-lv2\bin\gcc.exe`
- **Flags**: `-mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs`
- **Link**: `-mprx -nodefaultlibs -llv2_stub -lfs_stub -lnet_stub -lusbd_stub`
- **Signing**: `oscetool 0.9.2` via WSL, SELF type APP
- **PS3**: Cobra CFW, webMAN MOD 1.47.48c+, IrisMan
- **Injection**: PS3MAPI MODULE LOAD (`/ps3mapi.ps3?MODULE%20LOAD%200x{PID}%20{path}`)
- **Game**: LEGO Dimensions (BLES-XXXXX), official Sony SDK, scelibstub format

## What Works

1. **PS3MAPI MODULE LOAD** reliably loads the SPRX into the game process
   (PID 0x1010200). `module_start()` runs, worker thread created, init chain
   complete through `network_set_server()`.

2. **Trampoline generation**: `sys_memory_allocate(64KB)` succeeds, 4
   trampolines generated at runtime. No issues with R-W-X memory.

3. **Memory scanning is safe**: We probed and scanned the entire first LOAD
   segment (0x00010000 through 0x01B40000, ~27MB) without a single DSI crash.
   All pages in this range are readable.

4. **PS3MAPI injection is stable**: Single MODULE LOAD creates exactly one
   instance. No console crashes during injection.

5. **Diagnostics work**: Region probes log the first 4 words of each scan
   region before scanning. Per-region completion logging confirms which
   regions were fully scanned.

## What We Learned (Log Evidence)

### Finding 1: NID table is NOT in runtime memory

We scanned 4 regions covering the entire first LOAD segment:
```
Region 0: 0x00010000-0x00100000  probe: 0x7F454C46 (ELF header)     — code
Region 1: 0x00100000-0x00A00000  probe: 0x63BE0000 (PPC instructions) — code
Region 2: 0x00A00000-0x01400000  probe: 0x4182FF00 (PPC instructions) — code
Region 3: 0x01AE0000-0x01B40000  probe: 0x63830000 (PPC instructions) — code
```

**All 4 regions scanned completely. Zero NIDs found.** The cellUsbd NID values
(0x7F5F00D3, 0x1AB6D80B, 0x7B4436CE, 0x2F82F1A5) were not found in ANY of
the ~7 million words scanned.

The `.sceStub.rodata` which EBOOT.BIN analysis placed at vaddr 0x1B3ED80
(PH[6]) contains executable PowerPC code at runtime, not NID data. This
confirms the dynamic linker has either discarded or relocated the NID table
after resolving imports.

The triplet-format fallback scanner (`scan_nid_got_slot`) also found nothing —
the `{NID, reserved<0x1000, GOT_ptr}` pattern doesn't exist in runtime memory.

### Finding 2: boot_plugins.txt causes hard console crash

When the SPRX is in `/dev_hdd0/boot_plugins.txt`, Cobra loads it into VSH
processes at boot. The VSH worker thread probes 0x00010000 (game's ELF base),
which is unmapped in VSH → DSI → **entire PS3 freezes**, requiring filesystem
check and database rebuild. This happened twice. The DSI in a PPU worker
thread DOES kill the entire console, not just the thread.

**Conclusion**: boot_plugins.txt is not viable. PS3MAPI-only injection.

### Finding 3: PS3MAPI MODULE UNLOAD doesn't work

Repeated MODULE UNLOAD calls return `{"code": 200, "status": "OK"}` but the
SPRX stays loaded. The old instance continues holding UDP port 28472, blocking
new instances. After 10+ unload attempts before a new load, the old instance
still runs.

**Question**: Does PS3MAPI MODULE UNLOAD actually work for PRX files loaded
via MODULE LOAD? Or does it require a different identifier (module name vs
path)?

### Finding 4: OPD extraction returns import stubs, not real function addresses

```c
extern int cellUsbdInit(void);  // -lusbd_stub import
const ppc_opd_t *opd = (const ppc_opd_t*)(uintptr_t)cellUsbdInit;
// opd->code_addr = 0x7B0748 — this is the first instruction of the import stub,
// not a valid code address in PRX range (0x3xxxxxxx-0x4xxxxxxx)
```

The CellOS PRX import stubs are code snippets in our SPRX's `.text` section,
not OPD entries. Casting them to `ppc_opd_t*` reads the first instruction as
a fake "code_addr". All 4 cellUsbd imports fail validation (code_addr <
0x30000000).

### Finding 5: Existing PLT stub scanner not activated

The codebase already has `scan_plt_stubs()` in `usb_hooks.c` (lines ~820-890)
that searches for the standard PLT stub pattern:
```asm
lis   r12, got_hi      ; 0x3D80xxxx
lwz   r12, got_lo(r12) ; 0x818Cxxxx
mtctr r12              ; 0x7D8903A6
bctr                   ; 0x4E800420
```

This function is never called from `got_overwrite_one()`. It was written as
a diagnostic reference but never integrated into the hook installation flow.

## Expert Questions

### Q1: NID Table Location (CRITICAL)

At runtime in the LEGO Dimensions game process, where is the cellUsbd import
information stored? We've confirmed the NID values are NOT in any mapped
memory range (scanned 0x10000-0x1B40000 completely).

- Does the official Sony SDK dynamic linker discard `.sceStub.rodata` after
  import resolution?
- If so, where does the import information live at runtime? Is it only in
  the PLT stubs (`.plt` section) and GOT entries?

### Q2: PLT Stub Scanner — Correct Strategy?

The existing `scan_plt_stubs()` finds PLT stubs by pattern-matching the
16-byte `lis/lwz/mtctr/bctr` sequence. Once we have PLT stubs:

- **Q2a**: How do we match specific PLT stubs to the 4 cellUsbd functions
  (Init, OpenPipe, InterruptTransfer, ClosePipe)? We can't use NIDs
  (not in memory) and can't use OPD extraction (import stubs).

- **Q2b**: Is ordering reliable? If the game's import table lists
  cellUsbd functions consecutively, can we assume the first 4 PLT stubs
  with GOT values in the libusbd.sprx range are our targets?

- **Q2c**: Can we use the GOT slot VALUES to match? If we read each PLT
  stub's GOT slot and find 4 consecutive values that are all in the
  0x30000000-0x40000000 range and close together (same library), are
  those guaranteed to be cellUsbd?

### Q3: PLT Stub Location

Where do PLT stubs reside in the game's memory map? From our probe data,
all scanned regions (0x10000-0x1B40000) contain executable PowerPC code.
Would PLT stubs be:

- In a dedicated `.plt` section? At what typical offset?
- Interleaved with regular `.text` code?
- At a specific alignment boundary (e.g., 16-byte or 64-byte)?

### Q4: Do we even need to match functions?

Is it valid to just find ALL PLT stubs that point into libusbd.sprx and
overwrite all their GOT slots with trampolines? Our trampolines do
pass-through for non-ToyPad calls. The concern is:

- Would overwriting non-cellUsbd GOT slots in libusbd.sprx break the game?
- Are there other libusbd imports that we'd accidentally hook?

### Q5: PS3MAPI MODULE UNLOAD

Does `MODULE UNLOAD` work for PRX files loaded via `MODULE LOAD`? We see
`{"code": 200, "status": "OK"}` but the module stays loaded. Is there:
- A different endpoint or parameter format needed?
- A different identifier (module name string vs file path)?
- A known limitation of webMAN MOD 1.47.48c+?

### Q6: Build/Signing

Our SPRX is built with `-mprx` and signed as SELF type `APP`. Is this
compatible with PS3MAPI MODULE LOAD? Should we use a different SELF type
or PRX format for hot-injection into a running game process?

### Q7: Alternate Approaches

Given that NID scanning is a dead end and PLT stub matching may be fragile:

- **Q7a**: Could we use PS3MAPI `/read_process` to dump the game's GOT
  from the PC side, match entries offline, and then use `/write_process`
  to overwrite them? This avoids all in-process scanning.

- **Q7b**: Is there a CellOS syscall to enumerate a process's loaded
  modules and their import tables? (e.g., `sys_prx_get_module_list`)

- **Q7c**: Could we use the SPRX's own import resolution? When our SPRX
  calls `cellUsbdInit()`, the CellOS dynamic linker resolves it. Is there
  a way to capture the resolved address from within our own code?

### Q8: Console Freeze Diagnosis

When the SPRX loaded into VSH via boot_plugins.txt and the worker thread
probed 0x00010000 (unmapped), the entire PS3 froze requiring filesystem
check. We assumed this was a DSI, but:

- Does CellOS actually terminate only the faulting thread on DSI, or
  does it kill the process/console?
- Could the freeze be caused by something else (e.g., HDD I/O contention
  from 5+ simultaneous SPRX instances all writing to papertrail)?

---

## Files Referenced

- `sprx-plugin/usb_hooks.c` — NID scanner, PLT scanner, GOT overwrite
- `sprx-plugin/main.c` — module_start, VSH guard, worker thread
- `sprx-plugin/trampoline_gen.c` — Dynamic trampoline generator
- `analyze_eboot.py` — Offline EBOOT.BIN analysis
- `docs/HANDFF-2026-07-23-FINAL-BUILD.md` — Architecture overview
- `docs/EXPERT-DIAGNOSTIC-REPORT-2026-07-21.md` — webMAN API reference
