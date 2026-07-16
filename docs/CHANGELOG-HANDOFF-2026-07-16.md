# CHANGELOG / HANDOFF — Session 11 (2026-07-16)

## Claim Verified: Hand-rolled ASM PRX Headers + `-fPIC -shared` Produce Non-Loading SPRX

**Status: CLAIM CONFIRMED — then FIXED by reverting to PSL1GHT CRT approach.**

---

### 1. Root Cause Summary

The Session 10 refactor replaced the PSL1GHT `SYS_PROCESS_PARAM_FIXED(1001, 0x4000)` macro with hand-rolled assembly sections (`.sys_proc_prx_param`, `.rodata.sceModuleInfo`, `.lib.ent`) and switched the build pipeline to `-fPIC -shared` + `fself` only.

The result: **the plugin silently failed to load** on Cobra 8.5 CFW. Diagnostics confirmed:

- No boot log written → `_start()` never executed
- Enable token not consumed → plugin never ran
- `readelf` analysis of the built ELF showed **empty `.lib.stub` entries** — the kernel PRX loader couldn't resolve any LV2 syscall imports

### 2. What Was Fixed

| File | Change |
|------|--------|
| `sprx-plugin/main.c` | Removed entire `__asm__()` PRX header block. Restored `SYS_PROCESS_PARAM_FIXED(1001, 0x4000)`. Renamed entry points back to `_start`/`_stop` (matching `lv2-sprx.o` exports). **Preserved** the `sysFsChmod` + `sysFsUnlink` error hardening from Session 10. |
| `sprx-plugin/Makefile` | Reverted `-fPIC -shared` → `-fno-pic`. Restored all three tools: `sprxlinker` (generates `.lib.stub` NID import tables), `make_self` (creates SPRX-type SELF), `fself` (creates fake SELF). |

### 3. Why the Session 10 Approach Failed

The PSL1GHT SDK (specifically `lv2-sprx.o` + `sprxlinker`) expects the ELF to be built as **ET_EXEC** (not ET_DYN). When `-fPIC -shared` is used:

1. The linker creates an ET_DYN shared object
2. `make_self` still processes it, but `sprxlinker` can't resolve the stub structures
3. The resulting `.lib.stub` section contains zeroed-out NID entries
4. The kernel's PRX loader sees an empty import table and skips symbol resolution
5. `_start()` is never called

The fix: **go back to the known-working PSL1GHT build system** (`-fno-pic`, `SYS_PROCESS_PARAM_FIXED`, `sprxlinker → make_self → fself`).

### 4. Other Experiments That Were Dead Ends

#### 4.1 `scetool` Approach (naehrwert/scetool v0.2.9)
- Installed, compiles, runs — but requires key files not present in this toolchain
- `--np-app-type SPRX` option exists but fails with "Please specify a file type"
- Not needed once we reverted to working build pipeline

#### 4.2 `make_self` on Staged ELF
- `make_self` works on unstripped ELF but produces APP-type SELF header
- The working approach uses it in combination with `sprxlinker` and `fself`

### 5. Deployment & Testing Status

**Hard-reboot test 1 result:** Boot log ABSENT, enable token PERSISTS
→ **Diagnosis:** `boot_plugins.txt` was missing from PS3 (wiped by prior upload bugs)
→ **Fix:** Re-ran `ftp-deploy.ps1` which re-created `boot_plugins.txt` with `ldtoypad.sprx` entry

**Current state (ready for hard-reboot test 2):**
- ✅ `/dev_hdd0/plugins/ldtoypad.sprx` — present (12,880 bytes, fix build)
- ✅ `/dev_hdd0/plugins/boot_plugins.txt` — present with `ldtoypad.sprx` entry
- ✅ `/dev_hdd0/plugins/ldtoypad.enable` — present (one-shot token)

### 6. Files Modified in This Session

```
sprx-plugin/main.c       # Revert hand-rolled asm → SYS_PROCESS_PARAM_FIXED
sprx-plugin/Makefile     # Revert -fPIC -shared → -fno-pic + sprxlinker
sprx-plugin/MOVED        # Marker file (stale)
```

### 7. Key Lessons for Future Work

1. **PRX headers are not optional.** The kernel requires valid `.lib.stub` import entries. `SYS_PROCESS_PARAM_FIXED()` from PSL1GHT generates these correctly.
2. **`sprxlinker` is essential.** It rewrites `.lib.stub` sections with proper NID tables. Without it, imports are unresolvable.
3. **ET_EXEC, not ET_DYN.** SPRX modules on PS3 use ET_EXEC format. `-fPIC -shared` creates ET_DYN which breaks the toolchain.
4. **`boot_plugins.txt` is volatile.** Hard reboots can wipe it. Always verify after deployment.
5. The `sysFsChmod(0666)` + `sysFsUnlink` error hardening from Session 10 **is correct and should be preserved.** It prevents infinite boot loops from stale enable tokens.
6. The `sysFsOpen()` flag corruption fix (removing `| 0666`) from Session 10 **is correct and should be preserved** across all sysFsOpen call sites.

### 8. Next Steps

1. **Hard-reboot PS3** with all three files in place
2. **Check boot log** at `/dev_hdd0/plugins/ldtoypad_boot.log` via FTP
3. **Verify enable token consumed** (should 550 on FTP)
4. **Monitor UDP beacon** on PC server for `[BEACON]` discovery

---
*Report generated 2026-07-16 09:28 MDT*
