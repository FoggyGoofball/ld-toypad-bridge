# Expert Consultation — 2026-07-29: GOT Patching Proven, Harvester False Positive

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q [Full]
**Game:** LEGO Dimensions BLUS31473
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2 -fno-builtin -nodefaultlibs`
**SPRX:** ldtoypad.sprx (24,912 bytes v17), signed, PS3MAPI MODULE LOAD
**Status:** 🟢 GOT: 8/10 entries, 5/5 functions, NO crashes | 🔴 Harvester: FALSE POSITIVE | 🔴 Trigger: DSI every attempt

---

## Executive Summary

GOT patching is **proven and stable** — 8 of 10 entries across all 5
`cellUsbd*` functions with zero DSI crashes. The game runs perfectly.

The LddOps Harvester finds a candidate struct at `0x01E66C7C` with
name=`"desc"`, but this is a **false positive** — the name "desc" matches
a USB descriptor table, not an LDD ops struct. Calling through the "probe"
OPD at `0x01E0C100` always DSI-crashes (3 attempts with different stack/TOC
save strategies all failed identically).

The visual corruption seen before injection was likely caused by a stale
SPRX loading at boot via `boot_plugins.txt` and crashing.

**We need guidance on:**
1. Why the Harvester found a false positive despite TOC-matching + EBOOT-range checks
2. Whether to refine the Harvester or switch to Option 2 (XMB rescan via `cellUsbdRegisterLdd`)

---

## What Works (Proven)

- ✅ GOT pointer scanning: 8/10 entries, 5/5 functions, zero crashes
- ✅ cellFsWrite safe probe: scans 0x02000000-0xC0000000
- ✅ libusbd located: runtime base 0x02680000
- ✅ Synthetic OPD creation: ABI-compliant `{code, TOC, 0}`
- ✅ PowerPC cache flush: `dcbst/sync/icbi/isync`
- ✅ cellFsWrite page probing: prevents DSI on unmapped pages
- ✅ IPC file: proven format
- ✅ Deferred trigger (v16-v17): fires correctly at 3s into main loop

## What Failed

| Attempt | Result |
|---------|--------|
| cellUsbdGetDeviceDescriptor GOT hook | Patched but game never calls it |
| LDD ops scanner — name-based | False positive on "USB" |
| LDD ops scanner — heap range 0x02000000-0x04000000 | DSI crash |
| Harvester — EBOOT-only scan | Found struct, name="desc" (false positive) |
| call_game_opd v15 — init-time, no stack frame | DSI |
| call_game_opd v16 — deferred, stack frame, r11 save | DSI |
| call_game_opd v17 — deferred, stack save at 0x28, full clobber | DSI |

All `call_game_opd` attempts crash identically — log shows "Firing game
probe(0x99) manually..." then cuts off. Game process survives but SPRX
worker thread dies.

---

## Harvester False Positive Analysis

```
[USB] HARVESTER: LddOps at 0x01E66C7C name='desc' probe=0x01E0C100 attach=0x01E0C108
```

The struct at `0x01E66C7C` passes all Harvester checks:
- All 4 pointers in EBOOT range (0x00010000-0x02000000) ✓
- Probe and attach share the same TOC ✓
- Name starts with printable ASCII ✓

But the name `"desc"` is almost certainly a **USB descriptor table entry**,
not an LDD ops struct. The game likely has descriptor tables like:
```c
struct { char *name; void *desc_data; void *desc_data2; ... } usb_descriptors[];
```
This happens to match our 4-pointer pattern with same-TOC by coincidence.

### Why Every `call_game_opd` Crashes

We're calling `*(uint32_t*)0x01E0C100` as a function — but `0x01E0C100` is
data (a USB descriptor), not code. The PPU tries to execute descriptor bytes
as PowerPC instructions and hits an illegal instruction → DSI.

---

## libusbd OPD Table Reference (38 entries)

```
 idx  code_offset  first_insn   notes
   0   0x0010       0xF821FF91   (startup)
   1   0x0120       0x81228000   cellUsbdInit?
   2   0x0244       0x81228000   cellUsbdOpenPipe         ← GOT-patched
   3   0x0380       0x81228000   cellUsbdClosePipe        ← GOT-patched
   4   0x04B4       0x81228000   cellUsbdInterruptTransfer ← GOT-patched
   5   0x061C       0x81228000   cellUsbdGetDeviceDescriptor ← GOT-patched
   6   0x07C8       0x81228000   cellUsbdControlTransfer   ← GOT-patched
   7   0x0944       0x81228000   cellUsbdRegisterLdd?      ← NOT GOT-patched
   8   0x0A8C       0x81228000
   9   0x0BB4       0x81228000
  10   0x0D00       0x81228000
  11   0x0E38       0x81228000
  12   0x0F5C       0x2C240000   (different prologue)
  13   0x1264       0x2F840000
  ...
```

GOT scan targets (runtime OPDs at libusbd base 0x02680000):
```
TARGET_OPENPIPE   = 0x02680244
TARGET_TRANSFER   = 0x026804B4
TARGET_CLOSEPIPE  = 0x02680380
TARGET_GETDEVDESC = 0x0268061C
TARGET_CTRLXFER   = 0x026807C8
```

---

## Questions for Expert

### Q1: Is the Harvester candidate a false positive?

Struct at `0x01E66C7C`: `{name="desc"@0x01XXXXXX, probe=0x01E0C100, attach=0x01E0C108, detach=0x01E0C110}`

All 4 pointers in EBOOT range, probe/attach share TOC. But:
- Name `"desc"` looks like USB descriptor metadata, not an LDD name
- Calling `probe_opd` crashes every time (data, not code)
- Direct match for "USB" substring found previously

Should the Harvester require additional validation? E.g.:
- Name must be >= 4 printable chars
- detach_opd TOC must match probe/attach TOC
- code_addr must look like actual PPC instructions (not data)

### Q2: Should we switch to Option 2 (XMB Rescan)?

The expert previously suggested:
1. GOT-patch `cellUsbdRegisterLdd` (offset `0x0944` from libusbd, runtime OPD = `0x02680944`)
2. Hook it to capture the ops pointer from `r3`
3. Use XMB overlay to trigger USB stack rebuild → game calls RegisterLdd again

Does LEGO Dimensions use `cellUsbdRegisterLdd` (catch-all, offset 0x0944) or
`cellUsbdRegisterExtraLdd` (filtered, VID/PID-specific)? If the game uses
`RegisterExtraLdd`, we can't GOT-patch it since it's not in libusbd.

If we add `0x02680944` to our GOT scan, will we find GOT entries for it?
The game already imports 5 cellUsbd functions — a 6th seems plausible.

### Q3: Is there a way to validate OPD targets before calling?

Before calling `call_game_opd`, could we verify the target is actually
executable code by checking:
- The first instruction opcode is valid PPC
- The page is mapped executable (not just readable)
- Some other heuristic?

### Q4: boot_plugins.txt stale SPRX

Visual corruption appeared before injection — likely `boot_plugins.txt`
loads a stale SPRX at boot that crashes. We'll remove it, but is there
anything else that could cause pre-injection corruption?

---

## Build Info

```
SDK: DUPLEX 3.40
SPRX: 24,912 bytes signed (152,608 bytes unsigned)
Injection: PS3MAPI MODULE LOAD via webMAN HTTP
Wait: 5s (was 60s)

Key files:
  sprx-plugin/usb_hooks.c      — libusbd finder, GOT scanner, Harvester, 5 hooks
  sprx-plugin/trampoline_gen.c — 64-byte PPC trampoline generator
  sprx-plugin/main.c           — worker thread, call_game_opd, deferred trigger
  sprx-plugin/usb_hooks.h      — hook state (includes ldd_ops_addr field)
  ld-toypad-server/scripts/inject-sprx.js — Node.js orchestrator (5s wait)
```
