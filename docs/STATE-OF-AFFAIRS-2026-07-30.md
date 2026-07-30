# State of Affairs & Expert Consultation — 2026-07-30 (Final)

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q [Full]
**Game:** LEGO Dimensions BLUS31473 (22.6 GB ISO)
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2 -fno-builtin -nodefaultlibs`
**Stub libs:** -llv2_stub -lfs_stub -lnet_stub -lusbd_stub -lsysmodule_stub
**SPRX:** ldtoypad.sprx (~25 KB), signed via oscetool, injected via PS3MAPI MODULE LOAD
**Status:** 🟡 EBOOT v3 ISO ready for live test | 🔄 Ghidra + RPCS3 analyzing in parallel

---

## Executive Summary

We are attempting to build a LEGO Dimensions ToyPad emulation bridge that runs on a PC and communicates with a modified PS3 game. The core challenge is the PS3 kernel's USB VID/PID filter: it only delivers USB devices matching registered LDDs to the game process. The ToyPad VID/PID (0x0E6F/0x0241) is blocked unless the kernel sees a matching registration.

After exhausting SPRX-based approaches (registration hooks can't beat the T+5s race, XMB rescan never triggers RegisterLdd, USB hotplug blocked by kernel filter), we pivoted to patching the game's EBOOT to register a SanDisk VID/PID (0x0781/0x5581) instead of the ToyPad's. The PC bridge server emulates a SanDisk USB device.

The approach has evolved through three EBOOT builds:
- **v1 (PID-only):** Boots past update screen → black screen (VID 0x0E6F unmatched, device matching engine breaks)
- **v2 (Wrong VID offset):** Corrupted ori instruction → freeze at update screen
- **v3 (VID+PID):** Both VID (0x0781) and PID (0x5581) set at call site → ISO built, **awaiting live hardware test**

---

## What Works (Proven Across 10+ Sessions)

| Component | Status | Details |
|-----------|--------|---------|
| GOT pointer scanning | ✅ | 14/14 entries (8 hooks × 2), zero crashes |
| libusbd base finder | ✅ | 0x02680000 confirmed every session |
| Synthetic OPD creation | ✅ | ABI-compliant {code, TOC, 0} |
| PowerPC cache flush | ✅ | dcbst/sync/icbi/isync sequence |
| IPC file format | ✅ | Node.js orchestrator reads successfully |
| Registration hooks (×3) | ✅ | ppc_opd_t passthrough, expert-reviewed |
| call_game_opd (v17+) | ✅ | Stack-save at 0x28, full clobber list |
| EBOOT decryption | ✅ | oscetool decrypt to 29,849,712 byte ELF |
| EBOOT hex patching | ✅ | VID/PID bytes at known offsets |
| EBOOT re-signing | ✅ | oscetool 0.9.2 with template approach → 28.5 MB SELF |
| ISO → JB folder extraction | ✅ | Windows mount + robocopy |
| ISO rebuilding | ✅ | makeps3iso from ps3iso-tools |
| webMAN cache clearing | ✅ | /refresh.ps3?xmb fixes stale cache freezes |
| SPRX ldd_driver.c | ✅ | SanDisk VID/PID (0x0781/0x5581) |
| SPRX registration hook VID/PID swap | ✅ | Swaps arg2/arg3 before passthrough |

## What Failed

| Approach | Result | Root Cause |
|----------|--------|------------|
| Early SPRX injection (T+16-49s) | Hooks never catch RegisterLdd | Game registers at T+5s — injection too slow |
| XMB rescan trigger | Registration hooks silent | Game doesn't call RegisterLdd after overlay |
| USB hotplug with ToyPad VID/PID | GetDeviceDescriptor never called | Kernel filter blocks before reaching game |
| Harvester (v17–v24) | ZERO valid ops candidates | CellUsbdLddOps has non-standard layout |
| PS3MAPI MEMORY GET/SET | All zeros / Error 451 | webMAN API can't access game process memory |
| EBOOT v1 (PID-only) | Black screen after boot | VID 0x0E6F breaks device matching at 0x264080 |
| EBOOT v2 (wrong VID offset) | Freeze at update screen | Corrupted ori instruction |

---

## Current EBOOT v3: The Critical Test

### What's Patched

```
File offset 0x286DEC: 38800781  (li r4, 0x0781)   ← was: 63440000 (ori r4, r4, 0) [identity NOP]
File offset 0x286DF0: 38A05581  (li r5, 0x5581)   ← was: 38A00241 (li r5, 0x0241)
```

Both values are set immediately before `bl 0x264080` — the shared device matching function called from 5 locations.

### Why v1 (PID-only) Black-Screened

The `ori r4, r4, 0` at 0x286DEC is an identity NOP — it preserves whatever r4 the caller's caller set. With the PID-only patch, r4 arrives containing 0x0E6F (ToyPad VID) from somewhere 4+ levels up the call chain. The device matching engine at 0x264080 receives r4=0x0E6F, r5=0x5581 — a mismatched VID/PID pair that doesn't correspond to any real device. The function likely hits an assertion, null dereference, or fatal branch.

v3 fixes this by explicitly setting r4=0x0781 (SanDisk VID) at the call site.

### Why v3 Could Still Fail

The function at 0x264080 is a shared device matching engine called from 5 different locations — it's not ToyPad-specific. Our caller at 0x286DFC is the only one that uses the `ori r4, r4, 0` NOP pattern; the other 4 callers copy r4 from r29/r31, suggesting their VID comes from a data structure. If 0x264080 expects r4 to be a *pointer* to a device descriptor rather than a raw VID value, our `li r4, 0x0781` would corrupt the pointer and cause a crash.

This is why we're running Ghidra — to decompile 0x264080 and understand what it actually does with r4.

### ISO Location

```
C:\temp\LEGO_Dimensions_v3.iso  (22.33 GB)
EBOOT at LBA 3952
```

---

## Device Matching Function at 0x264080 — Under Investigation

### Known Facts

- Called from 5 locations: 0x2869E8, 0x286BAC, 0x286DFC (ours), 0x28762C, 0x287B90
- Our call site is unique: uses `ori r4, r4, 0` (NOP); others use `mr r4, r29` or `mr r4, r31`
- First 256 bytes show complex control flow — no VID/PID literals present
- Function body is part of a 56KB+ megafunction (likely inlined C++ from the game engine)
- The VID value 0x0E6F appears NOWHERE as an immediate in the entire 29MB ELF — it comes from runtime data

### Ghidra Status

```
Process: java.exe (PID 11572)
CPU: 976 seconds (~16 min)
RAM: 311 MB
Project: C:\temp\ghidra_proj\EBOOT
Status: Still importing — no .gbf/.db files created yet
Expected: 10-30 minutes for 29MB ELF auto-analysis
```

### RPCS3 Status

```
Status: Downloaded, attempting PPU module pre-compilation
Issue: 570 PPU modules to compile — extremely slow
Workaround: Switch to PPU interpreter mode (no compilation needed)
```

---

## The SPRX: Current State

### ldd_driver.c

Registers with CellOS using SanDisk VID/PID (0x0781/0x5581) — this is the SPRX's own LDD registration, separate from the game's. This would matter if we pivoted to having the SPRX handle USB entirely independently of the game's USB stack.

### usb_hooks.c

Three RegisterLdd hooks at GOT offsets 0x0944, 0x0BB4, 0x0D00. Each hook:
1. Checks `(uint32_t)(uintptr_t)arg2 == 0x0E6F && (uint32_t)(uintptr_t)arg3 == 0x0241`
2. If match: swaps to 0x0781/0x5581 before calling the original function
3. Logs to IPC file for Node.js orchestrator

**Critical limitation:** These hooks only fire if the SPRX is injected BEFORE the game calls RegisterLdd. Best measured injection time is T+16s; game registers at ~T+5s. The hooks have never caught a registration in any session.

### 8 GOT Hooks

| Function | GOT Offset | Purpose |
|----------|-----------|---------|
| OpenPipe | 0x244 | Intercept pipe open to ToyPad |
| Transfer | 0x4B4 | Intercept interrupt transfers |
| ClosePipe | 0x380 | Intercept pipe close |
| GetDevDesc | 0x61C | Intercept device descriptor read |
| CtrlXfer | 0x7C8 | Intercept control transfers |
| RegLdd@0944 | 0x0944 | VID/PID swap + capture ops |
| RegLdd@0BB4 | 0x0BB4 | VID/PID swap + capture ops |
| RegLdd@0D00 | 0x0D00 | VID/PID swap + capture ops |

Trampoline page: 0x11720000 (64KB R-W-X via sys_memory_allocate)

---

## Live Hardware Test Plan

### Setup

1. Copy `C:\temp\LEGO_Dimensions_v3.iso` → `E:\PS3ISO\LEGO Dimensions (USA) (En,Fr,Es).iso`
2. Clear webMAN cache: navigate to `http://192.168.0.22/refresh.ps3?xmb`
3. Delete any stale install data: FTP to `/dev_hdd0/game/` and remove `BLUS31473*` folders
4. Hold L2 during boot (disables Cobra plugins — eliminates one variable)

### Test Sequence

1. Launch game from webMAN XMB Games menu
2. If "A new version is available" appears: press Circle to cancel
3. If game reaches main menu / "Connect ToyPad" screen: **SUCCESS** — EBOOT patch works
4. Inject SPRX via webMAN: `http://192.168.0.22/plugin.ps3?mode=1&path=/dev_hdd0/tmp/ldtoypad.sprx`
5. Start bridge server on PC: `node ld-toypad-server/index.js`
6. Watch for IPC output: GOT hooks should fire when game opens USB pipes

### Expected Outcomes

| Result | Interpretation | Next Action |
|--------|---------------|-------------|
| Boots past update → black screen | v3 same as v1: device matching engine broken | Wait for Ghidra decompilation of 0x264080 |
| Freezes at update screen | FSELF DRM deadlock (not our patch) | Try different oscetool flags or template |
| Reaches "Connect ToyPad" | EBOOT patch WORKS | Inject SPRX, test full USB chain |
| Game crashes with error | VID/PID mismatch in kernel vs game logic | Analyze crash for clues |

---

## Open Expert Questions

### Q1: Is the function at 0x264080 a device matching engine or something else?

Our static analysis shows 5 callers, complex control flow, no VID/PID literals. We're running Ghidra to decompile it. But if anyone has prior knowledge of TT Games' LEGO engine USB initialization — is this a generic `FindDeviceByVIDPID(devList, vid, pid)` or is r4 something other than a raw VID?

### Q2: If v3 fails, is the SPRX in-memory patching approach viable?

Plan C: Boot original unmodified ISO → inject SPRX at T+60s → SPRX scans EBOOT .text in RAM for VID/PID instruction bytes → overwrites them → flushes icache. Does the PS3 MMU protect .text as read-only? Can `sys_memory_get_page_attribute` / `sys_memory_set_page_attribute` make it writable from a SPRX?

### Q3: Can we register a SanDisk LDD from the SPRX BEFORE the game registers its ToyPad LDD?

If we inject at T+2s (before the game's T+5s registration), can our SPRX call `cellUsbdRegisterLdd` with SanDisk VID/PID and have the kernel deliver a matching SanDisk-emulated device to US instead of the game? Would the game's subsequent RegisterLdd call for ToyPad fail gracefully?

This is the race condition we've been losing. Is there a faster injection method than PS3MAPI MODULE LOAD via webMAN HTTP? Could we use a Cobra payload, a VSH plugin, or boot_plugins.txt to load our SPRX at kernel boot time (T+0s)?

### Q4: What does the PS3 kernel actually do with USB VID/PID registration?

We know the kernel filters USB devices based on registered LDD VID/PID pairs. But:
- Does it deliver already-connected devices to newly registered LDDs?
- Does it require a physical disconnect/reconnect cycle?
- Is there a syscall to force USB re-enumeration?
- Does the filter apply to ALL USB classes or only HID?

### Q5: Can the bridge server actually emulate a convincing SanDisk USB device?

The PC bridge needs to present as a SanDisk USB device (VID 0x0781, PID 0x5581) to the PS3. Does this require:
- A specific USB device class (Mass Storage? HID? Vendor-specific?)
- Specific endpoint configurations?
- A particular USB descriptor layout?

The game may probe the device beyond just VID/PID matching and reject an imposter.

---

## File Inventory

| File | Size | Purpose |
|------|------|---------|
| `C:\temp\LEGO_Dimensions_ORIGINAL.iso` | 22.6 GB | Clean backup — DO NOT MODIFY |
| `C:\temp\LEGO_Dimensions_v3.iso` | 22.33 GB | v3 EBOOT (VID+PID patched) — READY FOR TEST |
| `C:\Users\Admin\EBOOT.elf` | 29,849,712 B | Decrypted original EBOOT |
| `C:\Users\Admin\EBOOT_v3.elf` | 29,849,712 B | Patched ELF (VID+PID) |
| `C:\Users\Admin\EBOOT_v3.BIN` | 29,834,608 B | Signed SELF for ISO |
| `C:\temp\ghidra_proj\` | — | Ghidra project (analyzing) |
| `docs\EXPERT-CONSULTATION-2026-07-30-ALL-EXHAUSTED.md` | — | Previous comprehensive doc |
| `docs\EXPERT-CONSULTATION-2026-07-30-EBOOT-FREEZE.md` | — | Previous EBOOT freeze doc |
| `docs\STATE-OF-AFFAIRS-2026-07-30.md` | — | THIS DOCUMENT |

---

## Decision Point

This document represents the current state before the live hardware test. If v3 fails:

1. **Wait for Ghidra** to decompile 0x264080 — understand what r4 actually is
2. **Try RPCS3 with interpreter mode** — set breakpoint at 0x296DF0 (runtime VA), read r4
3. **Pivot to Plan C:** SPRX in-memory patching of the running game's .text segment
4. **Consider Plan D:** Boot-time SPRX loading via boot_plugins.txt or VSH plugin (T+0s injection to beat RegisterLdd race)
5. **Last resort:** Accept that this game's EBOOT cannot be patched this way and explore completely different approaches (e.g., kernel-level USB filter manipulation, Cobra payload, or hardware ToyPad passthrough)

The SPRX infrastructure (GOT hooks, IPC, bridge server) is battle-tested and ready. The only missing link is getting the kernel to deliver our emulated USB device to the game process. Whether that happens through EBOOT patching, in-memory patching, or boot-time injection — the bridge will work once that link is established.
