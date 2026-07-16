# CHANGELOG / HANDOFF — 2026-07-16 v5

## Session Objective
Complete architectural remediation of `ldtoypad.sprx` — a PS3 Cobra 8.5 CFW VSH plugin that bridges a LEGO Dimensions Toy Pad over UDP to a Node.js server. The plugin was suffering from a **silent initialization failure**: `module_start()` was never reached (no boot log), the enable token was never consumed, and the module remained dormant despite compiling successfully.

---

## Diagnosis Summary

### Symptoms (Before This Session)
- Plugin compiled as ELF/SPRX (12,848 bytes)
- Deployed to `/dev_hdd0/plugins/ldtoypad.sprx`, registered in `boot_plugins.txt`
- PS3 booted normally, LEGO Dimensions ran — but **no diagnostic output appeared**
- Enable token (`/dev_hdd0/plugins/ldtoypad.enable`) was never consumed
- Boot log was never written (both at `/dev_flash/tmp/` and `/dev_hdd0/plugins/`)

### Root Causes Identified & Fixed

#### 1. Boot Log Path: `/dev_flash/tmp/` is READ-ONLY on Cobra CFWs
- **File:** `main.c`
- **The problem:** `boot_log_write()` wrote to `/dev_flash/tmp/ldtoypad_boot.log`. On Cobra CFW, `dev_flash` is typically mounted read-only (or the tmp dir does not exist at VSH plugin load time). `sysFsOpen()` fails silently (the return code is not checked with a diagnostic), so no log is ever written.
- **The symptom:** Every diagnostic line — `"[BOOT] module_start() called"`, `"[BOOT] ENABLE FLAG NOT FOUND -- dormant"`, etc. — was silently dropped by the OS. The plugin appeared completely dead.
- **The fix:** Changed boot log path to `/dev_hdd0/plugins/ldtoypad_boot.log` — the same directory the plugin lives in, which is guaranteed to be mounted and writable.

#### 2. Hand-Rolled PRX Metadata Collision with lv2-sprx.o
- **File:** `main.c` (removed inline assembly blocks), `crt0.S` (rewritten)
- **The problem:** `main.c` contained inline assembly that manually declared `.lib.ent`, `.rodata.sceModuleInfo`, and `.lib.stub` sections. This directly collided with the stock `lv2-sprx.o` startup object that PSL1GHT provides, which already defines the correct `.sys_proc_prx_param` section (magic `0x1B434CEC`) with proper `R_PPC64_ADDR32` relocations. The manual assembly used `.long _start` which truncated the 64-bit PowerPC function descriptor into 32 bits.
- **The fix:**
  - Removed all hand-rolled PRX metadata assembly from `main.c`
  - Rewrote `crt0.S` to only provide: `_start` (clean entry), `.lib.ent` exports (`module_start`/`module_stop`), and empty `.lib.stub` markers
  - Added `lv2-sprx.o` to LDFLAGS so the linker provides the correct `.sys_proc_prx_param` block

#### 3. crt0.S Cleanup — Removed Broken .opd and Duplicate Sections
- **File:** `crt0.S`
- **The problem:** The original `crt0.S` attempted to define `.opd` (AIX-style function descriptor table) which PSL1GHT's PPU ABI does **not** use — PowerPC64 ELF on PS3 uses direct function pointers, not descriptors. It also tried to define its own `.sys_proc_prx_param` section which overlapped with `lv2-sprx.o`.
- **The fix:** Removed `.opd` section, removed duplicate `.sys_proc_prx_param`, replaced `.word` with `.4byte` for 32-bit values (`.word` assembles to 16-bit on PPC), fixed `.size` directive that crossed section boundaries.

#### 4. NO -fPIE or ELF Relocation Changes
- **Status:** Explicitly AVOIDED (per user directive)
- The standard PSL1GHT model for VSH SPRX plugins is correct: `-fno-pic`, `ET_EXEC` output, bare `strip`, no `-pie` or `-Wl,-q`. The kernel PRX loader maps modules through `.sys_proc_prx_param` metadata and NID import/export tables, not ELF relocations.
- **Do NOT add `-fPIE`, `-pie`, `-Wl,-q`, or `--strip-unneeded`** in future sessions.

#### 5. LV2 Syscall Hooks — Eliminated
- **Status:** Zero instances of `sys_lv1_peek`/`sys_lv1_poke` or any LV2 memory patching remain in the codebase.
- Fully migrated to `sysUsbdRegisterExtraLdd()` in `ldd_driver.c` — firmware-agnostic USB interception via native CellOS LDD subsystem.

---

## Current State (End of Session)

### Build Output
| Metric | Value |
|--------|-------|
| File size | 12,848 bytes |
| Section headers | 17 |
| Build result | 0 errors (sprxlinker + make_self OK) |
| Compiler flags | `-fno-pic -std=gnu99 -fvisibility=hidden -D__SPRX__` |
| Linker flags | `-nostartfiles -nostdlib -nodefaultlibs` + `lv2-sprx.o` |

### Files Modified This Session
| File | Changes |
|------|---------|
| `sprx-plugin/main.c` | Boot log path changed from `/dev_flash/tmp/` → `/dev_hdd0/plugins/`; removed hand-rolled PRX metadata inline assembly; fixed comment about `sysFsOpen` mode argument |
| `sprx-plugin/crt0.S` | Complete rewrite: removed `.opd` section, removed duplicate `.sys_proc_prx_param`, fixed `.word`→`.4byte`, removed cross-section `.size` directive |
| `sprx-plugin/Makefile` | Added `lv2-sprx.o` to LDFLAGS, added `-std=gnu99`, added extensive documentation comments about why PIE is not used |
| `sprx-plugin/debug.c` | Injected physical file I/O to `/dev_hdd0/plugins/ldtoypad_debug.log` in `debug_ring_write()` for remote debugging |

### Deployed Files on PS3
| File | Path |
|------|------|
| `ldtoypad.sprx` | `/dev_hdd0/plugins/ldtoypad.sprx` |
| `ldtoypad.fake.self` | `/dev_hdd0/plugins/ldtoypad.fake.self` |
| Registered in | `/dev_hdd0/vsh/etc/boot_plugins.txt` |

---

## Persistent Issues — Still NOT Working

### Boot Log Is NOT Being Written
After rebuilding, redeploying, cold-booting the PS3, launching LEGO Dimensions, and exiting back to XMB — **the boot log `/dev_hdd0/plugins/ldtoypad_boot.log` does not exist** on the PS3.

**Possible reasons (in order of likelihood):**

1. **`module_start()` is never called by the kernel PRX loader.**
   - The module's `.lib.ent` export table or `.sys_proc_prx_param` metadata may still have a structural issue that causes the kernel to skip calling `module_start()`.
   - Note: the PS3 does NOT crash — LEGO Dimensions runs fine. This rules out catastrophic metadata parsing failures (which would panic the kernel). The loader either silently skips the module or the metadata is valid enough to load but the entry point is not dispatched.

2. **Boot log "write" silently fails even to `/dev_hdd0/plugins/`.**
   - The `sysFsOpen()` call with `SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND` may be using incorrect flags. Check if the kernel needs `SYS_O_CREAT` with a different flag combination, or if the VSH plugin context restricts file system access.
   - Line 74-76 of `main.c`:
     ```c
     sysFsOpen(LDTP_BOOT_LOG_PATH,
               SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND,
               &fd, NULL, 0)
     ```
   - Try using just `SYS_O_CREAT | SYS_O_WRONLY` (without `SYS_O_APPEND`) and truncate mode.

3. **The kernel may not call `module_start()` for "dormant" VSH plugins.**
   - Cobra may defer `module_start()` invocation until a VSH event triggers it, or the plugin needs a specific PRX type flag in the metadata.

### Debugging Approaches for Future Agent

#### Approach A: Verify kernel loads the module at all
```c
// Add EARLIEST possible diagnostic — write a fixed string via the
// simplest possible syscall combo.  Skip sysFs entirely initially:
// try cellFsOpen which may have different permission characteristics.
// Or use sysLv2Syscall to write a known value to a debug register.
```

#### Approach B: Simplify to an absolute minimum test
Create a minimal SPRX that does NOTHING but write one log line:
```c
int module_start(u64 args) {
    // Try cellFsOpen / sysFsOpen combo
    // Try /dev_hdd0/ directly vs /dev_hdd0/tmp/
    // Try raw cellFsWrite with no libraries linked
    return SYS_PRX_START_OK;
}
```
If even this doesn't produce output, the problem is in the metadata/loader chain.

#### Approach C: Examine the generated .lib.ent and .lib.stub
Use `powerpc64-ps3-elf-readelf -x .lib.ent ldtoypad-strip.elf` to verify the export table is correctly formed. The NIDs (0xB109AFB0 for module_start, 0x0B1C1CAA for module_stop) must match what the VSH loader expects.

#### Approach D: Check if Cobra requires specific .sys_proc_prx_param flags
The `sceModuleInfo` structure has a `type` field — for SPRX plugins the type should be `0x04` (PRX). If `lv2-sprx.o` emits the wrong type, the kernel won't dispatch `module_start()`.

---

## Key Architecture Decisions (Preserve)

### DO Keep
- `-fno-pic` CFLAG (standard SPRX, no PIE)
- `lv2-sprx.o` in LDFLAGS for PRX metadata
- `Extra LDD` via `sysUsbdRegisterExtraLdd` (firmware-agnostic USB)
- Boot log at `/dev_hdd0/plugins/` path
- crt0.S with clean `.lib.ent` exports and proper `.4byte` NID entries

### DO NOT Use
- `-fPIE`, `-pie`, `-Wl,-q` (not applicable to VSH plugins)
- `--strip-unneeded` (bare `strip` is correct)
- `sys_lv1_peek`/`sys_lv1_poke` or any LV2 memory patching
- Hand-rolled `.sys_proc_prx_param` (let `lv2-sprx.o` provide it)

---

## Git State (End of Session)

### Changes to commit:
- `sprx-plugin/Makefile` — cleaned up, added lv2-sprx.o, documented no-PIE rationale
- `sprx-plugin/main.c` — boot log path fix, removed hand-rolled metadata
- `sprx-plugin/crt0.S` — complete rewrite
- `sprx-plugin/debug.c` — added file I/O logging
- Various `.ps1` diagnostic scripts (check-boot-log*.ps1, check-ftp*.txt)

### Committed to `origin/main` ✅
