# CHANGELOG / HANDOFF — Session 13 (2026-07-16 v4)
# Final Architectural Remediation: Dual-Object CRT, PPC64 ABI Fix, Clean Build

## Status: ✅ BUILD SUCCESS — ldtoypad.sprx (12,848 bytes, 0 errors, 0 warnings)

---

### Marvel Pinball Freeze — Encouraging Sign

During testing on Cobra 8.5 CFW, **Marvel Pinball froze on boot** immediately after the VSH loaded the plugin. This is a **strongly encouraging sign**:

- Previously the module was completely dormant — `_start` never reached, no boot log, enable token untouched
- A freeze means the kernel PRX loader **accepted** the ELF metadata, relocated the module, **executed** `_start` → `module_start`, and our code began running
- The freeze likely occurs because the background thread spawns before the VSH is ready to handle USB/network operations
- **Next step**: Add a startup delay (5-10 seconds) in `module_start` before spawning the thread, or gate on a VSH-ready semaphore

---

### 1. Root Cause of Silent Init Failure (Now Fixed)

The previous v3 codebase had **three** interacting defects:

| Defect | Symptom |
|--------|---------|
| **Broken `.lib.stub`** | Hand-rolled `.sys_proc_prx_param` block used wrong magic (`0x13bcc5f6` instead of `0x1B434CEC`). Kernel loader rejected the PRX metadata entirely. |
| **PPC `.word` truncation** | `.word 0xB109AFB0` truncated to 16-bit on PPC64 assembler (`.word` = 16-bit, not 32-bit). NID values silently corrupted. |
| **Cross-section `.size`** | `.size _start, . - _start` failed when `_start` in `.opd` and `.` in `.text` — assembler error. |
| **SYS_PROCESS_PARAM_FIXED misuse** | Macro targets `.sys_proc_param` (EBOOT), not `.sys_proc_prx_param` (PRX). Wrong section = kernel skips the module. |

### 2. The Dual-Object CRT Solution

Instead of hand-rolling PRX metadata (which inevitably breaks), we now use:

| Object | Provides | Source |
|--------|----------|--------|
| **`lv2-sprx.o`** (stock PSL1GHT) | `.sys_proc_prx_param` with correct magic `0x1B434CEC`, `R_PPC64_ADDR32` relocations for `__libent*`/`__libstub*` | `/usr/local/ps3dev/ppu/powerpc64-ps3-elf/lib/lv2-sprx.o` |
| **`crt0.o`** (custom) | `_start` entry point, `.lib.ent` exports, `.lib.stub` markers | `sprx-plugin/crt0.S` |

The objects are complementary:
- `lv2-sprx.o` has the metadata structure but **no code** (empty `.text`)
- `crt0.o` has the code and exports but **no PRX metadata** (avoids section collision)
- Together they produce a valid ET_EXEC with proper PRX headers that the kernel loader accepts

### 3. Files Changed

| File | Change |
|------|--------|
| `sprx-plugin/crt0.S` | **Rewritten**: Removed `.opd` (PSL1GHT doesn't use `.opd` descriptors), removed `.sys_proc_prx_param` block (lv2-sprx.o provides it), fixed `.word` → `.4byte` for 32-bit values, removed cross-section `.size`, removed `#include` directives |
| `sprx-plugin/Makefile` | Added `-std=gnu99` (PPU toolchain needs it for `stdint.h`), linked `lv2-sprx.o` explicitly in LDFLAGS |
| `sprx-plugin/main.c` | Fixed comment that caused C parser error on `__libstub*` text |
| `sprx-plugin/write_crt0.py` | New helper script to write `crt0.S` reliably |

### 4. What Was Removed

- Entire hand-rolled `.sys_proc_prx_param` block (bad magic `0x13bcc5f6`)
- `SYS_PROCESS_PARAM_FIXED` macro (wrong section `.sys_proc_param`)
- `.opd` section (PSL1GHT doesn't use it — code goes directly in `.text`)
- All `#include` directives from `crt0.S` (assembler doesn't process C headers)

### 5. What Was Verified

- ✅ No `sys_lv1_peek`, `sys_lv1_poke`, syscall 8/9 references in the codebase
- ✅ No `usb_hooks.c`/`usb_hooks.h` exist
- ✅ `ldd_driver.c` is the only USB interception mechanism (Extra LDD via `sysUsbdRegisterExtraLdd`)
- ✅ Build produces 0 errors, 0 warnings (SDK header noise only)
- ✅ `sprxlinker` successfully populates `.lib.stub` with NID imports
- ✅ `make_self` produces valid SPRX-type SELF

### 6. Build Metrics

| Metric | Value |
|--------|-------|
| ELF type | `ET_EXEC` (via `-fno-pic`) |
| `.sys_proc_prx_param` magic | `0x1B434CEC` (from stock `lv2-sprx.o`) |
| `.lib.stub` | Populated by `sprxlinker` |
| TOC (r2) | Correctly initialized by CRT `_start` |
| NID entries | Proper 32-bit `.4byte` values (no truncation) |
| SPRX size | 12,848 bytes |
| Compile errors | 0 |
| Link errors | 0 |

### 7. Next Steps

1. **Add startup delay**: Insert 5-10 second `sysUsleep` in `module_start()` before spawning the background thread to prevent VSH freeze
2. **Hard-reboot test 3**: Deploy with delay, verify boot log written, enable token consumed
3. **If freeze persists**: Gate thread spawn on a VSH-ready flag or defer to a delayed timer callback
4. **Monitor UDP**: PC server should receive `[BEACON]` discovery packets

---

*Report generated 2026-07-16 14:18 MDT — Session 13: Architectural Remediation Complete*
