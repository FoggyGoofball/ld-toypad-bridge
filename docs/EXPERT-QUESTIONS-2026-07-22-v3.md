# Expert Question — No IPC Write: VSH Guard Poison + OPD Crash

## Diagnostic Evidence

### File: `/dev_hdd0/plugins/ldtoypad_boot.log` (excerpt from bottom)
```
=== ldtoypad Full Integration: module_start ===
VSH GUARD: detected VSH context — unloading immediately
VSH GUARD: remove ldtoypad.sprx from /dev_hdd0/boot_plugins.txt
=== ldtoypad Full Integration: module_start ===
VSH GUARD: detected VSH context — unloading immediately
VSH GUARD: remove ldtoypad.sprx from /dev_hdd0/boot_plugins.txt
```
(Repeated ~11 times — every injection fails)

### File: `/dev_hdd0/tmp/ld_vsh_guard.txt` (STILL EXISTS)
```
VSH_GUARD=1
```

### File: `/dev_hdd0/plugins/ldtoypad_debug.log` (excerpt)
```
[LDTP] [USB] Initializing USB hooks
[LDTP] [USB] Scanning game memory for cellUsbd NIDs...
```
Then **nothing** — thread silent death.

## Root Cause #1: VSH Guard Token Poisoning

**Code:** `sprx-plugin/main.c` — `module_start()`

```c
// Step 1: CHECK if guard exists
int exists_check = cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
                               CELL_FS_O_RDONLY, &scratch_fd, NULL, 0);
if (exists_check == CELL_OK) {
    cellFsClose(scratch_fd);
    papertrail("VSH GUARD: detected VSH context — unloading immediately");
    papertrail("VSH GUARD: remove ldtoypad.sprx from /dev_hdd0/boot_plugins.txt");
    return SYS_PRX_NO_RESIDENT;  // ← UNLOADS MODULE
}

// Step 2: CREATE guard (for NEXT boot)
cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
           CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC, &scratch_fd, ...);
```

**Bug:** The guard is written on injection #1. On injection #2 (same boot, no reboot), the guard still exists → `module_start()` immediately unloads. PS3MAPI still reports `{"code":200,"status":"OK"}` because `module_start()` was called — even though it instantly unloaded. **The injector interprets this as success.**

**Fix:** Delete the guard BEFORE checking it, not after:

```c
// DELETE old guard unconditionally first (from any previous injection this boot)
cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
           CELL_FS_O_WRONLY | CELL_FS_O_TRUNC, &scratch_fd, NULL, 0);
if (scratch_fd >= 0) cellFsClose(scratch_fd);

// NOW check: if guard already EXISTS after deletion → must be genuine VSH boot
// (because boot_plugins.txt re-creates it before module_start runs)
int exists_check = cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
                               CELL_FS_O_RDONLY, &scratch_fd, NULL, 0);
if (exists_check == CELL_OK) {
    cellFsClose(scratch_fd);
    // Genuine VSH boot — unload immediately
    return SYS_PRX_NO_RESIDENT;
}

// NOW create guard for next boot's genuine VSH detection
cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
           CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC, ...);
```

**QUESTION 1:** Is this the correct fix for the VSH guard poisoning? Or should the guard instead be written only once per **physical boot** by using a timestamp file or an LV2 syscall to get boot time? The problem is that injection #2+ in the same boot session hits the stale guard.

---

## Root Cause #2: OPD Extraction Crashes Silently

The debug log shows `[LDTP] [USB] Scanning game memory for cellUsbd NIDs...` and then **nothing** — the thread dies silently (no papertrail after that line, no IPC file written).

### The OPD Extraction Code (current):

```c
// usb_hooks.c — find_cellusbd_functions_via_opd()

extern int cellUsbdInit(void);     // -lusbd_stub import
extern int cellUsbdOpenPipe(...);
extern int cellUsbdInterruptTransfer(...);
extern int cellUsbdClosePipe(...);

typedef struct {
    uint32_t code_addr;    // Real function .text address
    uint32_t toc_addr;     // TOC base
    uint32_t env_ptr;      // 0
} ppc_opd_t;

static int find_cellusbd_functions_via_opd(void) {
    const ppc_opd_t *opd;
    uint32_t code_addr;

    opd = (const ppc_opd_t*)(uintptr_t)cellUsbdInit;
    code_addr = opd->code_addr;  // ← READS FIRST 4 BYTES OF IMPORT STUB

    // Range check: 0x30000000-0x4FFFFFFF
    if (code_addr < 0x30000000 || code_addr > 0x4FFFFFFF) {
        DEBUG_ERROR("[USB] OPD: cellUsbdInit code_addr out of range (0x%08X)\n",
                    (unsigned)code_addr);
        return -1;
    }
    ...
}
```

### The Prolem

When `-lusbd_stub` is used with `-mprx`, the symbols `cellUsbdInit`, `cellUsbdOpenPipe`, etc. are **PRX import stubs** in the SPRX's `.text` segment. Casting the import symbol to `ppc_opd_t*` does NOT give you an OPD — it gives you the first 4 bytes of the stub code (e.g., `0x7C0802A6` = `mflr r0`).

The actual OPD for the real `cellUsbdInit` function lives in **libusbd.sprx**'s data section (the exporting module). We cannot read it via a simple pointer dereference of our own import stub.

### The Fix We're Considering

Since OPD extraction via import stub casting doesn't work on PRX modules, options are:

**Option A:** Skip OPD extraction entirely. Just use `create_hook_trampoline()` with our C hook function pointers (which ARE real local OPDs). The trampolines are allocated in RWX memory, the Node.js orchestrator writes preambles targeting the trampoline addresses. Passthrough calls use direct `cellUsbdOpenPipe()` calls (which correctly go through the import stub).

**Option B:** Use NID scanning of the game's `.text` to find the game's calls to cellUsbd functions, then locate the game's GOT entries and overwrite them.

**Option C:** Use `sys_process_exit2` or `sys_mmapper_allocate_address` + `sys_mmapper_search_and_map` to get executable pages.

**QUESTION 2:** Is Option A safe (skip OPD extraction, let the dynamic trampolines work with our own function OPDs), or do we actually need to resolve the game's cellUsbd GOT entries? The architecture uses a Node.js orchestrator to write 4-instruction preambles into the game's .text — that process doesn't need any cellUsbd addresses from our SPRX. The SPRX only needs to:
1. Allocate RWX memory for trampolines ✓
2. Generate trampoline code ✓
3. Write IPC file with trampoline addresses ✓
4. Wait for Node.js to install preambles

None of these steps require the game's cellUsbd function addresses. Is that correct?

## Supporting Code Files (attached)

### `sprx-plugin/usb_hooks.c` (full)
Lines 501-582: OPD extraction that crashes
Lines 560-608: `usb_hook_init()` that calls OPD extraction then install_hooks() then write_ipc_file()

### `sprx-plugin/trampoline_gen.c` (full)
Dynamic trampoline generator — creates RWX executable code at runtime

### `sprx-plugin/main.c` (full)
Lines 65-134: `module_start()` with VSH guard logic
Lines 159-314: `worker_thread()` init chain

### `sprx-plugin/debug.c` (full)
Custom debug_printf that writes to HDD via cellFsWrite
