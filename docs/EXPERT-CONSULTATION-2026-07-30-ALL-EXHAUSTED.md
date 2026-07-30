# Expert Consultation — 2026-07-30: All Triggers Exhausted — Next Steps

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q [Full]
**Game:** LEGO Dimensions BLUS31473
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2 -fno-builtin -nodefaultlibs`
**SPRX:** ldtoypad.sprx v24 (25,744 bytes), signed, PS3MAPI MODULE LOAD
**Status:** ⚠️ All trigger methods exhausted | 🔴 Game install corrupted 3× by DSI crashes

---

## What Works (Proven)

| System | Status |
|--------|--------|
| GOT pointer scanning | ✅ 14/14 entries (8 hooks × 2 each), zero crashes |
| libusbd base finder | ✅ 0x02680000 confirmed across 10+ sessions |
| Synthetic OPD creation | ✅ ABI-compliant {code, TOC, 0} |
| PowerPC cache flush | ✅ dcbst/sync/icbi/isync |
| IPC file format | ✅ Proven, read by Node.js orchestrator |
| Registration hooks | ✅ 3 separate hooks, ppc_opd_t passthrough, expert-reviewed |
| call_game_opd (v17+) | ✅ Stack-save at 0x28, full clobber list |

## What Failed (All Approaches)

| Method | Result | Why |
|--------|--------|-----|
| **XMB rescan** (v20) | Registration hooks never fire | Game doesn't call RegisterLdd after overlay — only at startup |
| **USB hotplug** (v20) | GetDeviceDescriptor never called | Kernel VID/PID filter blocks device before reaching game |
| **Harvester v17** (4-ptr, same TOC) | 1 candidate: `0x01E66C7C`, name=`"desc"` | USB descriptor table false positive. `call_game_opd(0x01E0C100)` = DSI |
| **Harvester v21** (4-ptr, same TOC, stwu filter) | Same 1 candidate, rejected by stwu | Probe OPD contains descriptor data, not code |
| **Pair scan v22** (adjacent OPDs, stwu) | ZERO candidates in 32MB | No adjacent EBOOT OPD pairs with stwu prologues exist |
| **Individual OPD scan v23** (single OPD, stwu, TOC in EBOOT) | ZERO candidates | Game's OPDs may have TOC outside EBOOT range |
| **Individual OPD scan v24** (single OPD, stwu, no TOC filter) | DSI crash | Descriptor data at `0x01E0C100` dereferences to random addresses → unmapped memory |

## Key Finding

**The `CellUsbdLddOps` struct does NOT follow the standard Sony layout in this EBOOT.** After scanning all 32MB with four different strategies, the ONLY 4-pointer TOC-matched candidate is a USB descriptor table with name="desc". The real ops struct must have a non-standard arrangement — maybe separated probe/attach OPDs, different pointer ordering, or embedded in a larger struct.

## Questions for Expert

### Q1: Is the CellUsbdLddOps layout guaranteed?

The PS3 SDK defines:

```c
typedef struct {
    char     *name;
    void     *probe;    // OPD
    void     *attach;   // OPD
    void     *detach;   // OPD
    void     *suspend;  // OPD
    void     *resume;   // OPD
} CellUsbdLddOps;
```

But after scanning all 32MB of EBOOT with four different strategies, the ONLY candidate matching the first 4-pointer TOC-aligned pattern has name="desc" (USB descriptor table). The real ops struct either:
- Has non-contiguous OPD pointers (separated by other data)
- Uses a different struct layout (e.g., TT Games wrapper struct)
- Has probe/attach in a different order

### Q2: Is there another way to trigger the game's USB event loop?

Neither XMB rescan nor USB hotplug triggers any of our GOT hooks. The game's LDD was registered at startup (before injection) and sits idle waiting for the kernel to deliver a VID/PID-matched device — which never happens because the kernel filters everything.

Could we hook `cellUsbdInit` (libusbd offset 0x0120) or `cellUsbdScanDevice` to force enumeration? The libusbd OPD table has 38 entries — maybe one triggers device discovery.

### Q3: Can we read the ops pointer from libusbd's internal state?

When the game calls `cellUsbdRegisterLdd(ops)`, libusbd stores the ops pointer internally. If we could find where libusbd stores it (a global variable at a known offset within libusbd's .data), we could read it directly without needing to scan the EBOOT.

libusbd is at `0x02680000`. Are there known internal structures that hold the registered LDD list?

### Q4: Should we try injecting BEFORE the game reaches the ToyPad screen?

Currently we inject at the "Connect ToyPad" screen (game is already initialized). If we injected earlier (during game boot), the registration hooks would catch the initial `RegisterLdd` call. But injecting during boot is risky — the PS3MAPI might not be available, or the game might not have loaded libusbd yet.

### Q5: Should we fall back to the OPD hooks path?

The OPD hooks (`opd_hooks.c`) already exist in the codebase as a fallback. They use a different approach (stealing game callback pointers). After the LDD is already initialized, could the OPD hooks trigger the game's USB path somehow?

---

## Summary

We've proven GOT patching is bulletproof (14 entries, zero crashes across 10+ sessions). The registration hooks are architecturally correct (expert-reviewed). But we cannot find the ops struct in EBOOT (5 scan strategies failed) and cannot trigger the game's USB event loop (2 trigger methods failed).

**The bottleneck is not the hooking mechanism — it's finding the ops address and triggering the game's USB path.** We need guidance on where to look next.
