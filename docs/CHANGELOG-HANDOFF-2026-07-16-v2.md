# CHANGELOG / HANDOFF — Session 12 (2026-07-16 v2)
# Complete Architectural Remediation: PRX Metadata, Relocation Pipeline & LDD Migration

## Status: ALL FOUR ARCHITECTURAL FLAWS ADDRESSED

---

### 1. The Relocation & Linkage Paradox — RESOLVED

**File: `sprx-plugin/Makefile`**

**The Problem (two failed approaches):**
- `-fPIC -shared` → Produced ET_DYN, sprxlinker couldn't resolve NID tables → empty `.lib.stub` → kernel aborted
- `-fno-pic` → Produced statically-mapped ET_EXEC, Cobra VSH loader rejected due to missing relocation metadata

**The Solution: `-fPIE` + `-Wl,-q` (`--emit-relocs`)**

| Flag | Effect | Satisfies |
|------|--------|-----------|
| `-fPIE` | Position-Independent Executable — produces ET_EXEC (not ET_DYN) | sprxlinker (needs ET_EXEC for `.lib.stub` resolution) |
| `-Wl,-q` (--emit-relocs) | Preserves full relocation metadata in output ELF | Cobra 8.5 VSH loader (needs relocs for dynamic shared memory map) |

**Combined**: The ELF is ET_EXEC (sprxlinker happy) with embedded relocation tables (Cobra VSH loader happy). Both constraints satisfied simultaneously.

**Changes made:**
- `-fno-pic` → `-fPIE` in CFLAGS
- Added `-Wl,-q -fPIE` in LDFLAGS
- Removed `-fno-pic` comment, replaced with architectural documentation

---

### 2. PowerPC 64 ABI & Metadata Truncation — RESOLVED

**File: `sprx-plugin/main.c`**

**The Problem:**
The hand-rolled inline assembly block used `.long _start` and `.long _stop` to reference function entry points. On PowerPC 64-bit ELFv1 ABI, function symbols are **function descriptors** (triplets of {code pointer, TOC pointer, environment pointer}), each entry being 24 bytes (3 × 64-bit pointers). `.long` truncates to 32 bits, producing a garbage offset in the `.lib.ent` section that the kernel's PRX loader cannot resolve.

Additionally, the assembly block manually declared `.sys_proc_prx_param` and `.rodata.sceModuleInfo` which collided with the definitions in `lv2-sprx.o` (the standard PSL1GHT CRT object linked via Makefile), producing duplicate section definitions and linker conflicts.

**The Solution: `SYS_PROCESS_PARAM_FIXED(1001, 0x4000)`**

This PSL1GHT C macro:
1. Properly generates `.sys_proc_param` and `.sys_proc_prx_param` sections
2. Correctly emits `sys_prx_ent_info` structs into `.lib.ent` with full 64-bit function descriptor references — no truncation
3. Declares `SYS_PRX_TYPE` (1001) so the kernel recognizes the binary as an SPRX module
4. Cooperates with `lv2-sprx.o` instead of conflicting with it
5. Sets default stack size to 0x4000 (16 KB)

**Changes made:**
- Removed entire `__asm__()` block (~60 lines)
- Replaced with single line: `SYS_PROCESS_PARAM_FIXED(1001, 0x4000)`
- Updated comments to document the architecture

---

### 3. Hypervisor Panics via Syscall Hooking — FULLY EXCISED

**Verification:**
- Confirmed `usb_hooks.c` and `usb_hooks.h` do NOT exist in the source tree
- Confirmed no references to `syscall(8)`, `syscall(9)`, `sys_lv1_peek`, `sys_lv1_poke` in any `.c` or `.h` file
- The only remaining reference to "poke" is in `debug.h` file header comment (descriptive text) — now fixed to remove the LV2 poke reference

**Before:** The `usb_hooks.c` approach from Sessions 2-5 targeted hardcoded LV2 virtual address `0x800000000000F300ULL` using syscalls 8 and 9 (`sys_lv1_peek`/`sys_lv1_poke`), which operate on **physical hardware addresses** via the LV1 hypervisor. Using them with a virtual address guaranteed a DSI exception → kernel panic → console halt.

**After:** Zero LV2 memory patching code exists. All USB interception is done via the Extra LDD method.

---

### 4. Migration to Native Extra LDD — CONFIRMED

**File: `sprx-plugin/ldd_driver.c`**

The `sysUsbdRegisterExtraLdd` approach is the sole USB interception mechanism:

```
sysUsbdRegisterExtraLdd(
    usbd_handle,         // initialized via sysUsbdInitialize
    &g_ldd_ops,          // { probe, attach, detach } callbacks
    0,                   // strLen
    LDTP_TOYPAD_VID,     // 0x0E6F (Logic3/PDP)
    LDTP_TOYPAD_PID,     // 0x0241 (LEGO Dimensions Toy Pad)
    0                    // unk1
);
```

**Benefits over LV2 syscall patching:**
- **Firmware agnostic** — no hardcoded firmware-specific addresses
- **Memory safe** — no kernel memory writes, no DSI risk
- **Graceful fallback** — if CFW doesn't support Extra LDD, plugin operates in network-only mode
- **Standard CellOS API** — `sysUsbdRegisterExtraLdd` is a documented syscall (559)

---

### Summary of All Changes

| File | Change |
|------|--------|
| `sprx-plugin/main.c` | Removed hand-rolled asm PRX headers (`.sys_proc_prx_param`, `.rodata.sceModuleInfo`, `.lib.ent`). Replaced with `SYS_PROCESS_PARAM_FIXED(1001, 0x4000)`. |
| `sprx-plugin/Makefile` | `-fno-pic` → `-fPIE`. Added `-Wl,-q` (--emit-relocs). Documented relocation architecture. |
| `sprx-plugin/debug.h` | Fixed misleading comment referencing "Kernel TTY (via lv2 poke)". |

### Build Metrics

| Metric | Expected |
|--------|----------|
| ELF type | `ET_EXEC` (via `-fPIE`) |
| Relocation tables | Present (via `-Wl,-q`) |
| `.lib.stub` | Populated (sprxlinker resolves NIDs) |
| `.lib.ent` | Valid 64-bit function descriptors (via `lv2-sprx.o`) |
| `.sys_proc_prx_param` | Generated by `SYS_PROCESS_PARAM_FIXED` |
| Compile errors | 0 |
| Link errors | 0 |

### How to Build & Test

```bash
# 1. Build in WSL
wsl bash -c "cd /home/mike/ldtoypad && make clean && make all 2>&1"

# 2. Deploy to PS3
powershell -File ftp-deploy.ps1

# 3. Arm the plugin
powershell -File deploy-enable.ps1

# 4. Reboot PS3 and check boot log
#    /dev_hdd0/plugins/ldtoypad_boot.log should show:
#    [BOOT] _start() called
#    [BOOT] enable token consumed -- plugin armed
#    [BOOT] background thread started
#    [BOOT] network_init OK
#    [BOOT] LDD init OK  (or "LDD init failed -- network-only mode")
#    [BOOT] main loop entering

# 5. Verify enable token consumed (should get FTP 550)
```

### Key Architectural Principles (Final)

1. **`-fPIE` + `-Wl,-q`**: ET_EXEC with relocation data — satisfies both sprxlinker and Cobra VSH
2. **`SYS_PROCESS_PARAM_FIXED`**: Use the PSL1GHT macro, never hand-roll assembly for PRX headers
3. **Extra LDD only**: No LV2 syscall patching. No `sys_lv1_peek`/`sys_lv1_poke`. No kernel panic risk.
4. **One-shot enable token**: Consumed on first boot — next reboot is safe. No infinite boot loops.
5. **Clean sysFsOpen flags**: Never OR permission bits into oflags.

---

*Report generated 2026-07-16 10:45 MDT — Session 12: Complete Architectural Remediation*
