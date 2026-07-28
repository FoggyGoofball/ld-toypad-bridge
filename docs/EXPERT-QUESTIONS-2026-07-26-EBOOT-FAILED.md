# Expert Question — 2026-07-26: EBOOT Patching Failed, Next Steps

## Status

We have:
- 4 OPD hooks perfectly working (RegisterLdd, RegisterExtraLdd, ControlTransfer, BulkTransfer)
- UDP bridge, debug/network/LDD all functional
- Callback stealing logic ready — just need the game's `CellUsbdLddOps*` pointer

## What We Tried

### EBOOT Patching (Failed)
- Patched EBOOT.BIN with TrueAncestor v1.96, option 2 "Resign to NON-DRM EBOOT"
- Placed `ldtoypad.sprx` in the `self` subfolder as instructions indicated
- Deployed to PS3: game hangs at black screen (never reaches menu)
- Restored original EBOOT: game works normally

### PS3MAPI Injection with --wait 0 (Still Too Late)
- Inject as soon as PID detected (within ~500ms of game launch)
- RegisterLdd hook installed but game already called it
- `g_game_ops` stays NULL

### Memory Scanning for LddOps Struct (Crashes)
- Scanned game memory for `{name_ptr, probe_OPD, attach_OPD, detach_OPD}` pattern
- DSI crashes on unmapped pages — OPDs are dynamically allocated outside LOAD segments
- Can't find the struct without IDA/Ghidra

## Questions

1. **Was TrueAncestor option 2 the right option?** Should we have used option 7 "Custom Sign" or a different workflow? Is there a way to verify the SPRX import was actually added to the patched EBOOT before deploying?

2. **Could our SPRX be crashing during early boot?** Our `module_start` does papertrail filesystem writes and thread creation. Would a minimal SPRX that ONLY hooks RegisterLdd (no filesystem, no threads) survive early boot?

3. **Is there a `sce` tool that can add PRX imports from the command line?** We have oscetool 0.9.2 and scetool but neither supports adding PRX imports directly.

4. **Alternative: `boot_plugins.txt`?** We abandoned this early because it caused VSH freezes. But now our SPRX is cleaner (no memory probing at 0x00010000). Could we load via VSH boot_plugins.txt and have it auto-attach to the game process?

5. **Given our constraints (no IDA, working OPD hooks, RegisterLdd hook installed but late), is there ANY other way to get the game's LddOps pointer?**
