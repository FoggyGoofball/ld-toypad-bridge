# Expert Handoff: No IPC Write — Root Cause Analysis

## Crash-Before-Hooks: The Three Suspects

### Suspect #1 (MOST LIKELY): VSH Guard Token Poisoning
**File:** `sprx-plugin/main.c`, `module_start()`

**Bug:** The VSH guard token (`ld_vsh_guard.txt`) is WRITTEN on the first successful injection, and **never deleted**. On subsequent injections (without reboot), the guard file still exists, so the SPRX mistakenly identifies itself as running in VSH context and calls `return SYS_PRX_NO_RESIDENT` — the module unloads immediately.

**Execution path:**
```
Injection 1:
  module_start()
  → ld_vsh_guard.txt: not found (first time)
  → writes ld_vsh_guard.txt
  → thread created, everything works
  → hooks installed, IPC written ✓

Injection 2 (minutes later, same boot):
  module_start()
  → ld_vsh_guard.txt: EXISTS ← from injection 1!
  → "VSH GUARD: detected VSH context — unloading immediately"
  → return SYS_PRX_NO_RESIDENT
  → module unloaded, thread NEVER runs
  → IPC file NEVER written
```

PS3MAPI says `{"code":200,"status":"OK"}` because the SELF file was loaded and `module_start` was called — even though it immediately unloaded itself. **The injector misreports success.**

**Fix:** Delete the guard file at the START of module_start, BEFORE checking it:
```c
// DELETE old guard unconditionally first
cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
           CELL_FS_O_WRONLY | CELL_FS_O_TRUNC, &scratch_fd, NULL, 0);
if (scratch_fd >= 0) cellFsClose(scratch_fd);

// Now check — if it (somehow) re-exists after deletion, then VSH
```

### Suspect #2: OPD Range Check ABEND
**File:** `sprx-plugin/usb_hooks.c`, `find_cellusbd_functions_via_opd()`

The range check `code_addr < 0x30000000 || code_addr > 0x4FFFFFFF` is wrong. If `-lusbd_stub` causes the GOT to resolve to the **exporting module's OPD** (whose code_addr points to real LV2 kernel code at 0x8XXXXXXX), the check fails and the function returns -1.

This returns -1 gracefully, BUT: the `DEBUG_ERROR()` call inside the if-block triggers `debug_printf()` → which calls `cellFsOpen()` to write to `/dev_hdd0/plugins/ldtoypad_debug.log` — if the cellFsWrite during papertrail is already open from a previous call, the nested cellFsOpen might deadlock the filesystem, hanging the thread.

### Suspect #3: `sys_memory_allocate` with invalid flags
**File:** `sprx-plugin/usb_hooks.c`, `install_hooks()`

`SYS_MEMORY_PAGE_SIZE_64K` might not be a defined constant in the Sony SDK headers used. If it's undefined, it evaluates to 0, which maps to `SYS_MEMORY_PAGE_SIZE_1M` — allocating a full 1MB instead of 64KB, potentially exhausting the game's memory. But more critically: if the flag is wrong, the kernel might reject the allocation entirely, returning an error code that causes the trampoline base to be 0, and then the trampoline writes would crash.

---

## Immediate Diagnostic: Pull the Boot Log
Run this to see exactly where the worker thread died:
```
curl -u mike:mike ftp://192.168.0.47/dev_hdd0/plugins/ldtoypad_boot.log
```

Also pull the debug log:
```
curl -u mike:mike ftp://192.168.0.47/dev_hdd0/plugins/ldtoypad_debug.log
```
