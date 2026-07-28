# Expert Question — 2026-07-24: Vector A Implementation Blocked

## Context

We confirmed: no NIDs, no PLT stubs, no GOT entries for cellUsbd in the
decrypted EBOOT.elf (29.8MB). The game uses OPD tables instead of PLT.

We attempted Vector A (VID/PID search) but hit a blocker.

## What We Tried

Searched decrypted ELF for ToyPad VID (0x0E6F) and PID (0x0241) in all
byte orders (BE16, LE16, BE32 combined, LE32 combined).

- **PID (0x0241)**: 5,774 matches — useless, it's a common PowerPC instruction
  byte (part of `cmpwi`, `li`, `b` etc.)
- **VID (0x0E6F)**: 7 matches, all false positives:
  - 4 in code segment (PH[0]): part of PowerPC `lis`/`addis` immediate values
  - 3 in data segment (PH[1]): part of the game's OPD function pointer table
    (code address 0x010E6F70 — bytes 0E 6F just happen to be there)
- **Combined VIDPID (0x0E6F0241)**: Zero matches
- **USB device descriptors (bLength=18, bDescriptorType=1)**: 125 found, none
  with VID=0x0E6F

The game does NOT store the ToyPad VID/PID as standard USB descriptor data
in the ELF.

## Question

1. **Vector A — what exactly should we search for?** Since the VID/PID aren't
   in standard USB descriptor format, how do we locate the ToyPad init
   function? Should we search for:
   a) The VID as a 16-bit immediate in a `li`/`cmpwi` instruction pattern?
   b) The VID/PID encoded differently (maybe as part of a config struct with
      different field ordering)?
   c) Something else entirely — like cellUsbd syscall numbers (0x20A-0x20F)
      in `li r11, N` / `sc` instruction pairs?

2. **Vector B — is LV2 syscall hooking practical?** If Vector A requires
   too much reverse engineering per game update, should we pivot to hooking
   the LV2 USB syscalls (sys_usbd_*) via a Cobra kernel payload? This would
   work regardless of how the EBOOT calls USB functions.

3. **Is there a middle ground?** For example, since our SPRX is already
   injected and has `-lusbd_stub` linked, could we:
   a) Register as a USB LDD via `cellUsbdRegisterExtraLdd`?
   b) Hook `cellSysmoduleLoadModule` for `CELL_SYSMODULE_USBD` to intercept
      when the game loads the USB module?
