# LD-ToyPad Bridge Handoff Changelog (2026-07-15, Session 6-9)

## Resume Marker: PRX Header Assembly & Kernel Load Remediation

### Diagnostic Summary
The persistent symptom triad — **no boot log, no debug log, untouched failsafe token** — proved that `_start()` was never called. The PS3 kernel was rejecting the plugin during initial parsing, long before any C code executed.

### Root Causes Found (Session 9)

1. **EBOOT Process Conflict**: `SYS_PROCESS_PARAM_FIXED(1001, 0x4000)` injected a `.sys_proc_param` section marking the binary as a standalone executable. Cobra 8.5 tried loading it as a VSH module, but the kernel rejected it due to container mismatch.

2. **Missing PRX Export Stubs (.lib.ent)**: GCC `__attribute__((visibility("default")))` exposes symbols to the linker but does NOT create the `sys_prx_ent_info` structs required by the PS3 PRX loader. Without these, Cobra found an empty export table and abandoned the plugin.

### Changes Applied

### 1. `sprx-plugin/main.c` — PRX Header Assembly Rewrite (Session 9)
- **Removed**: `SYS_PROCESS_PARAM_FIXED(1001, 0x4000)` — standalone EBOOT marker, caused kernel reject
- **Added**: Raw inline assembly block containing three sections:
  - `.sys_proc_prx_param` — PRX metadata for sprxlinker (preserved from previous asm)
  - `.lib.ent` with `0x01300000` struct — registers `_start` as module entry point
  - `.lib.ent` with `0x01400000` struct — registers `_stop` as module exit hook
- **Preserved all previous fixes**: `| 0666` permission masks, `sysFsChmod()`, token unlink enforcement, disk debug I/O

### 2. `sprx-plugin/main.c` — Permission & Token Fixes (Session 6)
- **`boot_log_write()`**: Added `| 0666` to `sysFsOpen()` flags — files were created with `0000` permissions
- **`boot_log_write_fmt()`**: Same `| 0666` fix for the second sysFsOpen call
- **`check_enable_flag()`**: Complete rewrite —
  - Added `sysFsChmod(0666)` before unlink to override FTP client permissions
  - Captures `sysFsUnlink()` return value
  - **Enforces failsafe**: if unlink fails, `return 0` (dormant) with boot log message
  - Prevents infinite boot loop from surviving token

### 3. `sprx-plugin/debug.c` — Disk I/O Restoration (Session 6)
- Added `#include <lv2/sysfs.h>`
- **`debug_ring_write()`**: Injected `sysFsOpen` → `sysFsWrite` → `sysFsClose` to
  `/dev_hdd0/plugins/ldtoypad_debug.log` with `| 0666` mask

### 4. `sprx-plugin/main.c` — Visibility Fix (Session 5)
- Added `__attribute__((visibility("default")))` to `_start()` and `_stop()`
  to override `-fvisibility=hidden` in Makefile

### 5. `ftp-deploy.ps1` — CRLF Fix (Session 5)
- `boot_plugins.txt` now written with LF-only line endings to avoid Cobra parser issues

---

## Build Metrics

| Session | Change | Size | Errors |
|---------|--------|------|--------|
| 5 | `SYS_PROCESS_PARAM_FIXED` + visibility + asm | 12,672 B | 0 |
| 6 | `| 0666` + `sysFsChmod` + disk I/O | 12,880 B | 0 |
| 9 | Remove `SYS_PROCESS_PARAM_FIXED`, add `.lib.ent` stubs | **12,752 B** | 0 |

---

## Verification Status

| Check | Result |
|-------|--------|
| SPRX compiles with native PSL1GHT API (0 errors) | ✅ 12,752 bytes |
| `.sys_proc_prx_param` section present | ✅ |
| `.lib.ent` export stubs for `_start`/`_stop` | ✅ |
| `| 0666` on all `sysFsOpen` calls | ✅ |
| `sysFsChmod` + enforced unlink in `check_enable_flag()` | ✅ |
| Disk I/O in `debug_ring_write()` | ✅ |
| `boot_plugins.txt` LF-only line endings | ✅ |
| Pushed to GitHub (`main@503c797`) | ✅ |
| **Hard-reboot test** | ⏳ Pending |

---

## How to Test

```bash
# 1. Deploy SPRX + token (already done)
powershell -File ftp-deploy.ps1
powershell -File deploy-enable.ps1

# 2. Hard-reboot PS3 (hold power until red light, wait 5s, power on)

# 3. Check diagnostics via FTP:
#    - /dev_hdd0/plugins/ldtoypad_boot.log
#    - /dev_hdd0/plugins/ldtoypad_debug.log
#    - /dev_hdd0/plugins/ldtoypad.enable (should be consumed)
```

## Deployment Files

| File | Purpose |
|------|---------|
| `sprx-plugin/build/ldtoypad.sprx` | Compiled SPRX (12,752 bytes) |
| `sprx-plugin/build/ldtoypad.fake.self` | Fake SELF for Cobra load |
| `deploy-enable.ps1` | FTP one-shot enable token |

---

## Next Steps After Hard-Reboot

1. Verify `ldtoypad_boot.log` exists and shows boot sequence
2. Verify `ldtoypad.enable` is consumed (FTP 550 error)
3. Launch Lego Dimensions → verify toy pad bridge connections
4. If successful, second reboot should show "dormant exit" in boot log

---

## Key Files Summary

| File | Purpose | Last Modified |
|------|---------|--------------|
| `sprx-plugin/main.c` | PRX entry, assembly header + export stubs, token gate, boot log | 2026-07-15 (Session 9) |
| `sprx-plugin/debug.c` | Ring buffer + file I/O + UDP remote debug | 2026-07-15 (Session 6) |
| `sprx-plugin/ldd_driver.c` | Extra LDD registration via `sysUsbdRegisterExtraLdd` | 2026-07-14 |
| `sprx-plugin/network.c` | UDP transport, discovery beacons | 2026-07-14 |
| `ldbuild.sh` | Windows→WSL build bridge | 2026-07-14 |
| `ftp-deploy.ps1` | SPRX + `boot_plugins.txt` LF-normalized deployment | 2026-07-15 |
| `deploy-enable.ps1` | FTP one-shot enable token | 2026-07-15 |
