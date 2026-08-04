# Project Handoff Report — LD-ToyPad Bridge
## Why the Steam Deck approach works but the SPRX plugin never did

**Date:** 2026-08-03  
**Project:** LEGO Dimensions ToyPad Emulation for PS3  
**Final working solution:** Steam Deck USB Gadget + Berny23 LD-ToyPad-Emulator  
**Abandoned approach:** PS3 SPRX plugin + EBOOT patching + UDP bridge

---

## Executive Summary

After months of reverse engineering, we successfully emulated the LEGO Dimensions ToyPad on a PS3 using a Steam Deck in USB gadget mode running Berny23's LD-ToyPad-Emulator. Our earlier approach — injecting a custom SPRX plugin into the game process and patching the EBOOT — failed completely. This report explains why.

**The core insight:** We were trying to circumvent the PS3 kernel's USB filter from the wrong side. Berny23's approach goes through the filter (by impersonating the real hardware), while we tried to go around it (by modifying the game's registration). The kernel filter is a hard barrier that cannot be bypassed from userspace.

---

## Phase 1: The SPRX Plugin Approach (Failed)

### What we built

- A custom SPRX (`ldtoypad.sprx`, ~25KB) injected via PS3MAPI MODULE LOAD
- 8 GOT hooks intercepting `libusbd` functions at runtime
- A UDP bridge server on PC that supplied ToyPad response data
- A Node.js orchestrator reading IPC files from the SPRX

### What worked

| Component | Status | Details |
|-----------|--------|---------|
| GOT pointer scanning | ✅ | 14/14 entries (8 hooks × 2), zero crashes across 10+ sessions |
| libusbd base finder | ✅ | 0x02680000 confirmed every session |
| Synthetic OPD creation | ✅ | ABI-compliant {code, TOC, 0} |
| PowerPC cache flush | ✅ | dcbst/sync/icbi/isync |
| IPC file format | ✅ | Node.js orchestrator reads successfully |
| SPRX injection | ✅ | Loaded reliably via webMAN HTTP API |

### What failed and why

#### 1. The Registration Race (FATAL)

The game calls `cellUsbdRegisterLdd()` at approximately **T+5 seconds** after launch. PS3MAPI MODULE LOAD injection completes at **T+16-49 seconds**. Our three RegisterLdd hooks (at GOT offsets 0x0944, 0x0BB4, 0x0D00) **never fired in any session** because the SPRX wasn't loaded yet when the game registered.

- XMB rescan: game doesn't re-register after initial boot
- USB hotplug: kernel filter blocks non-matching devices before they reach the game
- Early injection: physically impossible with PS3MAPI's loading speed

**Root cause:** The PS3's module loading mechanism is too slow. Boot-time plugin loading (boot_plugins.txt) might have worked but we never tested it.

#### 2. The VID Mystery (FATAL)

We spent weeks trying to find where the ToyPad VID (`0x0E6F`) is stored in the EBOOT. Our exhaustive binary search found:

- **0x0E6F appears exactly ONCE** in the entire 29MB ELF — as coincidental bytes in an unstructured data section
- **Not a single `li` instruction** anywhere in the ELF loads 0x0E6F
- The PID (0x0241) appears in 68 locations — mostly in instructions

**Root cause:** The VID comes from the USB device descriptor at runtime. The game reads it from the ToyPad hardware. It is **not hardcoded** anywhere in the EBOOT. We were searching for a ghost.

#### 3. The Slot Allocator Misidentification (FATAL)

We thought `bl 0x264080` at file offset 0x286DFC was calling a "device matching function" with r4=VID, r5=PID. Our static analysis via `ppu-lv2-objdump` revealed:

- The actual call target is **VA 0x274080** (file offset 0x264080 + segment base 0x10000)
- This function is a **device slot allocator** that scans 16 pre-allocated slots
- r4 is a **pointer passed to strcpy** — NOT a VID value
- r4=0 (NULL) is valid — strcpy copies an empty string
- Our `li r4, 0x0781` patch would dereference address 0x0781 → instant segfault
- The function at 0x16BE34C is **strcpy/memcpy** (classic 0x7F7F7F7F pattern)
- PID (r5=0x0241) is stored in the slot structure — that's the only static value

**Root cause:** We patched the wrong instruction with the wrong value, at the wrong address, based on a wrong understanding of what the code does.

#### 4. EBOOT Signing & Freezing

- Modified EBOOT freezes at "A new version is available" screen
- Caused by FSELF DRM metadata deadlock with VSH (confirmed by experts)
- webMAN cache made even original ISO freeze (fixed via /refresh.ps3?xmb)
- oscetool cannot reproduce Sony's proprietary SELF compression (28.5MB → 12.3MB)

#### 5. Incomplete ToyPad Protocol

Our `toypad-protocol.js` had:
- Basic HID report structure (8-byte + 80-byte)
- MIFARE tag data layout
- Character/token ID lookup
- **Missing:** Full NFC tag crypto (MIFARE Ultralight C authentication)
- **Missing:** LED color command handling
- **Missing:** Tag write/upgrade persistence
- **Missing:** Wake/probe/attach handshake

Berny23's `node-ld` library implements the **complete** protocol, including all 20+ ToyPad commands.

---

## Phase 2: Static Analysis (Educational but Unproductive)

### Ghidra

- Headless import of 29MB EBOOT.elf hung after 25+ minutes
- No database files produced
- Useful for confirming function boundaries but impractical for this codebase

### RPCS3

- PPU module pre-compilation stuck on 570 modules
- USB emulation too high-level — ToyPad appears "already connected"
- Game skips registration code path entirely
- Cannot reproduce the real PS3 kernel USB filter behavior

### ppu-lv2-objdump

- **The one tool that actually delivered results**
- Correctly identified the slot allocator function
- Revealed r4=0 (NULL pointer) at the call site
- Found all 5 callers of the slot allocator
- Confirmed 0x0E6F is not an immediate in any instruction
- Identified 0x16BE34C as strcpy (not a device matcher)

---

## Phase 3: The Steam Deck Solution (Works)

### Why it works

The Steam Deck's USB-C port in Dual-Role Device (DRD) mode can impersonate **any USB device** at the hardware level. By creating a USB gadget with:

- **VID 0x0E6F, PID 0x0241** — exact match for the LEGO Dimensions ToyPad
- **HID descriptor** — 32-byte INPUT, 32-byte OUTPUT (Array type)
- **Device strings** — "LEGO READER V2.10", "PDP LIMITED. ", "P.D.P.000000"
- **Report length** — 32 bytes

The PS3 kernel sees a **real ToyPad** and delivers it to the game. No EBOOT patches. No SPRX injection. No race conditions. No VID searching. No signing issues.

### Architecture

```
Steam Deck (USB Gadget)
  └─ /dev/hidg0 (raw HID endpoint)
       └─ Berny23/LD-ToyPad-Emulator (node-ld + Express + Socket.io)
            ├─ Full ToyPad protocol (all 20+ commands)
            ├─ NFC tag crypto (MIFARE Ultralight C)
            ├─ LED color/fade/flash effects
            └─ Web UI at http://localhost (touchscreen)
                  ↓ USB-C to USB-A cable
               PS3 (sees "LEGO READER V2.10")
                  └─ LEGO Dimensions (original, unmodified)
```

### Key differences from our approach

| Aspect | Our SPRX approach | Steam Deck approach |
|--------|------------------|---------------------|
| USB filtering | Tried to bypass kernel filter | Goes through filter natively |
| Game modification | Required EBOOT patches | Zero modifications |
| Timing | Race against T+5s registration | No timing dependency |
| Protocol | Partial (missing crypto, LEDs) | Complete (Berny23's node-ld) |
| VID source | Searched EBOOT for months | Hardware impersonation |
| HID descriptor | Assumed 80/8 byte reports | Correct 32/32 byte reports |
| Device strings | Guessed them | Exact match from real hardware |
| Deployment | Complex signing pipeline | Single curl command |

---

## Lessons Learned

### 1. Hardware impersonation beats software patching

The PS3 kernel's USB filter is designed to be a security boundary. It's intentionally hard to bypass from userspace. The correct approach is to present valid hardware that passes the filter — not to modify the software that registers with it.

### 2. Static analysis has limits with obfuscated C++

TT Games' engine uses massive inlined C++ megafunctions (56KB+). Without a working decompiler, tracing register values through virtual dispatch chains is nearly impossible. The `ori r4, r4, 0` NOP we found was not preserving a VID — it was preserving a pointer that happened to be NULL on the success path.

### 3. Complete protocol implementation matters

Our bridge had the basic HID report structure but was missing critical parts of the ToyPad protocol (NFC authentication, LED effects, write persistence). Even if we had gotten the USB delivery working, the game would have rejected our incomplete responses.

### 4. The right tool for the job

`ppu-lv2-objdump` gave us more actionable data in 5 minutes than weeks of Ghidra/RPCS3 setup. Sometimes the simplest tool is the best.

### 5. Know when to pivot

We spent months on EBOOT patching because "it should work in theory." The moment we confirmed the VID wasn't hardcoded, we should have pivoted immediately. The Steam Deck solution took 2 days from concept to working.

---

## Current State

| Component | Status |
|-----------|--------|
| `run.sh` | ✅ Vanilla Berny23, one-curl install |
| `run-ui.sh` | ✅ Custom UI overlay (Toy Box, modals, FIFO) |
| `sync-images.js` | ✅ Fandom wiki thumbnail downloader |
| `deck_toypad.sh` | ⚠️ Legacy, use run.sh or run-ui.sh |
| `start_toypad.sh` | ⚠️ Legacy auto-update script |
| `sprx-plugin/` | 🔴 Abandoned — will not work |
| `ld-toypad-server/` | 🔴 Abandoned — UDP bridge approach |
| EBOOT patches | 🔴 Abandoned — patching wrong code |

---

## Repository

All code at: https://github.com/FoggyGoofball/ld-toypad-bridge

Working entry points:
- `deck/run.sh` — vanilla Berny23 one-liner
- `deck/run-ui.sh` — custom UI version
- `deck/overlay/` — HTML/CSS/JS for custom UI
- `deck/README.md` — full setup guide
