# CHANGELOG / HANDOFF — 2026-07-18 — **THE BREAKTHROUGH**

## Session Objective
Achieve first successful Cobra VSH PRX plugin load on Evilnat 4.93 dual-BD. Diagnose and fix three root-cause bugs. Enshrine canonical solutions.

## STATUS: ✅ HELLO PLUGIN BUILDS, SIGNS, DEPLOYS, AND **BEEPS ON COLD BOOT**

---

## THE THREE BUGS (Canonical)

### Bug 1: `SYS_PRX_RESIDENT` (2) vs `return 0` — CRITICAL

**Symptom:** Cobra silently rejected the plugin. `module_start` was never dispatched.

**Root Cause:** Cobra's `modulespatch.c` parses `boot_plugins.txt` sequentially. It evaluates the return code of `module_start` to determine if it should proceed to the next plugin. A return value of `0` indicates the boot phase completed successfully. Returning `SYS_PRX_RESIDENT` (which evaluates to `2`) is interpreted as an abnormal exit state by Cobra's specific loader logic, triggering a silent abort to prevent a system-wide lockup.

**Fix:** Return `0` from `module_start`. The detached background thread keeps the logic resident anyway.

**Canonical Rule:** In Cobra VSH plugin context, `module_start` MUST return `0`. Do NOT return `SYS_PRX_RESIDENT`.

### Bug 2: Incomplete Asm Clobber List — CRITICAL

**Symptom:** The `sc` (system call) instruction corrupted compiler register state, causing undefined behavior in the surrounding function.

**Root Cause:** The original inline assembly only declared `"memory"` as a clobber. The `sc` instruction in PowerPC destroys ALL volatile registers: `r3` through `r12`, `r0`, `cr0`, `ctr`, `xer`, and `lr`. Without declaring these, the compiler may have held live values in those registers, leading to corruption after `sc` returns.

**Fix:** Declare the complete clobber list:
```c
: "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
  "r11", "r12", "r0", "cr0", "ctr", "xer", "lr"
```

**Canonical Rule:** EVERY `sc` inline asm block MUST declare ALL 15 volatile registers as clobbers. No exceptions.

### Bug 3: Initialization on Main Loader Thread — CRITICAL

**Symptom:** Cobra's VSH PRX loader requires that all plugin logic execute in a thread SEPARATE from the main loader thread. Running syscalls (`sc`) directly on the loader thread causes a race condition with the System Manager subsystem.

**Fix:** Spawn a detached PPU thread via `sys_ppu_thread_create()` in `module_start`, and execute all logic (including any `sc` instructions) in that thread. `module_start` itself must return `0` immediately after creating the thread.

**Canonical Pattern:**
```c
void worker_thread(uint64_t arg) {
    (void)arg;
    /* All syscalls, delays, and logic here */
    sys_ppu_thread_exit(0);
}

int module_start(size_t args, void *argp) {
    (void)args; (void)argp;
    sys_ppu_thread_t tid;
    sys_ppu_thread_create(&tid, worker_thread, 0, 0, 0x1000, 0, "worker");
    return 0;  /* IMMEDIATE return — NOT SYS_PRX_RESIDENT */
}
```

---

## COBRA DISABLED ITSELF (Major Time Sink)

**Discovery:** When a plugin crashes during boot, **Cobra disables itself** by writing its toggle to OFF in the system settings. This means all subsequent boots will NOT load ANY plugins, even if the crashing plugin is removed or fixed. The user must manually re-enable Cobra in:
```
Settings → Custom Firmware Tools → Cobra Tools → Enable Cobra → ON
```

This wasted many debug cycles because we kept deploying fixes into a system where Cobra was already OFF.

---

## LESSONS LEARNED (Migration Plan for sprx-plugin)

### 1. Use Sony SDK, NOT PSL1GHT
The Sony SDK's `-mprx` flag + linker script (`elf64_lv2_prx.x`) handles ALL PRX metadata generation. The macros `SYS_MODULE_INFO`, `SYS_MODULE_START`, `SYS_MODULE_STOP` from `<sys/prx.h>` generate `.rodata.sceModuleInfo`, `.lib.ent`, and `.lib.stub` sections correctly.

### 2. NO inline assembly for PRX metadata
With Sony SDK macros, remove ALL inline assembly blocks that manually define `.rodata.sceModuleInfo`, `.lib.ent`, `.lib.stub`. The SDK handles this automatically via `prx_crt0.o` and the linker script.

### 3. Link Sony stub libraries ONLY
- `-llv2_stub` (threading, syscalls)
- `-lfs_stub` (filesystem)
- `-lnet_stub` (networking)
- `-lsysmodule_stub` (sysmodule loading)
- Do NOT mix with PSL1GHT libraries (`-llv2`, `-lnet`) — ABI incompatibility.

### 4. Header mapping (PSL1GHT → Sony SDK)
| PSL1GHT | Sony SDK |
|---------|----------|
| `<lv2/sysfs.h>` | `<cell/cell_fs.h>` or `<sys/fs.h>` |
| `<sysmodule/sysmodule.h>` | `<cell/sysmodule.h>` |
| `<sys/thread.h>` + `<lv2/thread.h>` | `<sys/ppu_thread.h>` |
| `sysThreadCreate()` | `sys_ppu_thread_create()` |
| `sysLv2FsOpen()` | `cellFsOpen()` |

### 5. scetool flags (CANONICAL — MATCHES WORKING webftp)
```
--key-revision=000A
--self-auth-id=1010000001000003
--self-vendor-id=01000002
--self-app-version=0001000000000000
--self-type=APP
--self-ctrl-flags=4000000000000000000000000000000000000000000000000000000000000002
--self-fw-version=0004009300000000
```

---

## Files Changed This Session

| File | Change |
|------|--------|
| `hello-plugin/main.c` | **REWRITTEN**: Sony SDK style, thread delegation, full clobber list, return 0, 4-beep loop with delay |
| `hello-plugin/Makefile` | Added `-llv2_stub` to LDFLAGS |
| `docs/CHANGELOG-HANDOFF-2026-07-18.md` | This file — canonical documentation |

## Build Artifacts (Working)
- `hello-plugin/build/helloworld.prx` — 9,600 bytes (intermediate ELF)
- `hello-plugin/build/helloworld.sprx` — 4,800 bytes (signed SPRX)
- Both deployed to PS3 and confirmed functional

## Git State
Will commit as:
```
v8: BREAKTHROUGH — First working Cobra VSH SPRX plugin
- Bug 1: SYS_PRX_RESIDENT(2) → return 0 (Cobra interprets 2 as abort)
- Bug 2: Complete sc clobber list (all 15 volatile registers)
- Bug 3: Thread delegation (sys_ppu_thread_create for all syscalls)
- Discovery: Cobra disables itself after plugin crash
- 4-beep diagnostic version deployed and tested
```
