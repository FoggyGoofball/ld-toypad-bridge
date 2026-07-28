# Expert Question — 2026-07-26: Revised EBOOT Patching Architecture

## Previous Failure

TrueAncestor option 2 alone didn't inject the SPRX import (just resigns).
Our SPRX's heavy `module_start` (papertrail writes, network_init) crashed
early boot → black screen.

## New Architecture

### EBOOT Patching Pipeline
1. **Decrypt**: `oscetool -d EBOOT.BIN EBOOT.elf`
2. **Inject SPRX**: `SPRXPatcher EBOOT.elf /dev_hdd0/plugins/ldtoypad.sprx EBOOT_patched.elf`
   - SPRXPatcher output: `PPU hash: 10ef0fe20...`, patch successful
3. **Re-encrypt**: TrueAncestor option 2 "Resign to NON-DRM EBOOT"

### SPRX Changes for Early Boot Safety
```c
int module_start(size_t args, void *argp) {
    // NO filesystem, NO papertrail, NO network — just create thread
    sys_ppu_thread_create(&g_worker_tid, worker_thread, ...);
    return 0;  // return immediately
}

static void worker_thread(uint64_t arg) {
    sys_timer_usleep(15 * 1000 * 1000);  // 15 second delay
    // THEN: papertrail, debug_init, network_init, OPD hooks, LDD, main loop
}
```

The 15-second delay ensures the game's subsystems (sys_net, cellFs, USB) are
fully initialized before the SPRX touches them.

### Expected Flow
1. Game boots → EBOOT loads our SPRX (via SPRXPatcher-imported dependency)
2. `module_start`: create thread, return 0 (sub-millisecond)
3. Game continues booting normally (no blocking)
4. 15 seconds later → worker thread wakes → debug → network → OPD hooks
5. **RegisterLdd hook catches the game's CellUsbdLddOps** (game registered it during boot)
6. Server connects via UDP → fires game_ops->probe() / attach()
7. Game proceeds past "connect toypad"

## Questions

1. **Is the 15-second delay sufficient and safe?** Could `sys_timer_usleep(15s)` in a PPU thread cause issues during early boot? Should we use a polling approach instead (checking if cellFs is available)?

2. **Will `cellUsbdRegisterLdd` be called AFTER our 15-second delay?** The game might register USB within the first few seconds. If so, our RegisterLdd hook (installed via OPD scan at ~16s) would miss it again. Should we install the hook in module_start instead (no delay), but only do heavy init later?

3. **The OPD scan reads 0x00010000-0x04000000.** During early boot, are these pages fully mapped? Could the scan DSI-crash if run too early?

4. **Is TrueAncestor option 2 the correct resign method** for a SPRXPatcher-modified ELF? Or should we use `oscetool -e` directly with the same flags we use for SPRX signing (SELF type APP)?
