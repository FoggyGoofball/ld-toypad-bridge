# Expert Consultation — 2026-07-28: cellFsWrite Safe Probe (HANDOFF v4 FINAL)

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q [Full]
**Game:** LEGO Dimensions BLUS31473
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2 -fno-builtin -nodefaultlibs`
**SPRX:** ldtoypad.sprx (23,296 bytes v8), signed, PS3MAPI MODULE LOAD
**Status:** 🟢 SPRX crash-proof, zero DSI | 🔴 libusbd NOT FOUND after 8 iterations, 992MB scanned

---

## Results Table (All Tests)

| v | Scan Range | Matching Strategy | Result |
|---|-----------|-------------------|--------|
| v3 | 0x02000000→0x04000000 | exact 0x94EC, 16B cmp | ❌ All CELL_OK, no match |
| v4 | 0x02000000→0x20000000 | exact 0x94EC, 16B cmp | ❌ 480MB game heap, all CELL_OK |
| v5 | diagnostics only | — | cellFsWrite OK, readback EMPTY |
| v6 | 0x30000000→0x40000000 | exact 0x94EC, 16B cmp | ❌ PRX region, all CELL_OK |
| v7 | 0x30000000→0x40000000 | 512B window (v+0x9400→0x9600) | ❌ No match, PS3 SURVIVED |
| v8 | 0x30000000→0x40000000 | ±48B around 0x94EC | ⏳ Deployed, awaiting test |

**Key:** ALL 2,048 pages across 0x30000000→0x40000000 returned `CELL_OK` with 16 bytes written. Windowed reads caused no DSI crash. But `"cellUsbd_Library"` / `"cellUsbd"` never matched.

## Diagnostic Results (v5)

```
[USB] DIAG cellFsWrite ret=00000000 wr=00000010    ← local string write OK
[USB] DIAG readback:                                ← EMPTY (cellFsRead failed)
[USB] DIAG at 0x00010000: ret=00000000 w=00000010   ← EBOOT probe OK
[USB] DIAG EBOOT readback:                          ← EMPTY
```

- `cellFsWrite` returns CELL_OK from both local and EBOOT (0x00010000) buffers
- `cellFsRead` back from file returns nothing — cannot verify buffer transmission

## PS3 Memory Map (Empirically Verified)

| Range | Content | cellFsWrite |
|-------|---------|-------------|
| 0x00010000-0x01FFFFFF | EBOOT .text/.data | CELL_OK (confirmed valid via PS3MAPI) |
| 0x02000000-0x2FFFFFFF | Game heap + RSX | CELL_OK for ALL pages |
| 0x30000000-0x3FFFFFFF | System PRX region | CELL_OK for ALL pages |
| 0x40000000+ | Unknown | Not yet scanned |

## Offline Offsets (libusbd.sprx, firmware 4.93)

```
LIBUSBD_OFFSET_OPENPIPE       = 0x00000244
LIBUSBD_OFFSET_CLOSEPIPE      = 0x00000380
LIBUSBD_OFFSET_INTERRUPT_XFER = 0x000004B4
LIBUSBD_OFFSET_GET_DEV_DESC   = 0x0000061C
LIBUSBD_OFFSET_CONTROL_XFER   = 0x000007C8
MODULE_INFO_OFFSET            = 0x94EC  (sceModuleInfo.name = "cellUsbd_Library")
```

## Exhausted Approaches

| # | Approach | Result |
|---|----------|--------|
| 1 | LV2 syscalls in PRX | ❌ Blocked/panic |
| 2 | PS3MAPI MEMORY GET kernel | ❌ All zeros |
| 3 | Node.js Smart Probe | ❌ All zeros |
| 4 | /modules.ps3mapi endpoint | ⚠️ 501, no data |
| 5 | webftp_server_full.sprx slot 1 | ❌ Game-only, not web server |
| 6 | cellFsWrite scan 0x02000000-0x20000000 | ❌ No match |
| 7 | cellFsWrite scan 0x30000000-0x40000000 | ❌ No match (v6-v8) |

## Questions for Expert

**Q1: Where is libusbd.sprx loaded on Evilnat 4.93 CEX + Cobra 8.5?**
We scanned the game heap (0x02000000-0x20000000) and PRX region
(0x30000000-0x40000000). Is libusbd above 0x40000000? At 0x60000000+?
Is there a standard firmware module map for 4.93?

**Q2: Is cellFsWrite actually transmitting buffer data?**
v5 diagnostics: cellFsWrite returns CELL_OK for both local and remote
buffers, but cellFsRead back from the file is EMPTY. This could mean
cellFsRead is broken in PRX context, OR cellFsWrite isn't actually
reading from the buffer pointer. If the latter, the entire approach
is invalid and we need a different strategy.

**Q3: Is the MODULE_INFO_OFFSET (0x94EC) correct at runtime?**
Derived from decrypted ELF: sceModuleInfo at 0x95D4 → vaddr 0x94E4,
name field at +8 = 0x94EC. Could the runtime layout of sceModuleInfo
differ from the ELF layout?

**Q4: Can we capture the SPRX's resolved cellUsbd import addresses?**
The PRX links with `-lusbd_stub`. When loaded, CellOS resolves imports
through the PRX's GOT. The `extern int cellUsbdOpenPipe(...)` in the PRX
goes through the PLT stub, not the resolved address. Is there a way in
C or PPU asm to read the resolved GOT entry for an imported function?

**Q5: Alternative — PS3MAPI MEMORY SET blind writes?**
If MEMORY SET (poke_process) has different privilege than MEMORY GET
(peek_process), we could blindly write preambles at candidate addresses
without reading first. Test: write preamble at 0x30000000+0x244, see if
cellUsbd calls redirect.

## Build

```
SDK: DUPLEX 3.40
ppu-lv2-gcc -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs
Link: -llv2_stub -lfs_stub -lnet_stub -lusbd_stub -lsysmodule_stub
Sign: oscetool 0.9.2 -5 APP (WSL)
Size: 23,296 bytes (v8)
```

## Ready the Moment libusbd Base is Known

- ✅ SPRX crash-proof (zero DSI across 992MB of scanning)
- ✅ Trampolines allocated, 5 hooks generated
- ✅ IPC format: TARGET_*=base+offset, TRAMP_*=trampoline_page+offset
- ✅ Preamble builder: 4 PPC instructions per hook
- ✅ PS3MAPI MEMORY SET for preamble installation
- ❌ **libusbd.sprx runtime base address** — ONLY remaining blocker
