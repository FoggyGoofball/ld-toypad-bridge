# Expert Question — 2026-07-26: Locating USB Calls for Hooking in OPD-Based Game

## Context

The expert recommends hooking `cellUsbdInterruptReceive` as a MITM
strategy — let the game claim the ToyPad, intercept data at the
software layer. This is the right approach.

## The Blocker: Can't Find the Hook Target

The game (LEGO Dimensions, BLES-02175) uses **PPC64 ELFv1 OPD tables**
(Official Procedure Descriptors) for all function calls. We've
exhaustively searched and confirmed:

1. **Zero cellUsbd NIDs** in the 28.5MB LOAD segment — the dynamic
   linker discards `.sceStub.rodata` after resolution
2. **Zero PLT stubs** — the decrypted EBOOT.elf has zero
   `lis/lwz/mtctr/bctr` patterns for cellUsbd. The game calls USB
   functions through OPD tables: `{code_addr, toc_addr, env_ptr}`
3. **No GOT slots** — the game doesn't use lazy binding for cellUsbd
4. **VID/PID search failed** — 0x0241 matches 5,774 times (common
   PowerPC instruction bytes), VID/PID combined = 0 matches

## The OPD Problem

In a normal PLT-based game, you'd find `cellUsbdInterruptReceive` by:
- Scanning for its NID in the import table → find the stub → patch it

But this game uses OPD tables. The function pointer to
`cellUsbdInterruptReceive` is resolved at load time into an OPD entry
in the game's data section. The game doesn't call through a stub — it
loads the OPD address from its data table and branches through it.

We can't find the OPD entry because:
- The OPD is just a 16-byte struct `{code_addr, toc_addr}` — both
  values are runtime addresses unknown at compile time
- No NID survives in memory to search for
- No PLT stub pattern to scan for

## Questions

1. **Given OPD-based calling, how do we locate the game's
   `cellUsbdInterruptReceive` call site?** The code that calls it
   looks like:
   ```asm
   ld r12, cellUsbdInterruptReceive@toc(r2)  ; load OPD address from TOC
   ld r2, 8(r12)                              ; load TOC from OPD
   mtctr r12                                   ; move code addr to CTR
   bctrl                                       ; branch through OPD
   ```
   But we don't know the TOC offset or OPD address. Can we scan for
   the `bctrl` pattern near known USB-related code?

2. **Alternative: hook `sys_usbd` LV2 syscalls instead?** The game
   ultimately calls `sc` (syscall) instructions for USB operations.
   If we can find the `li r11, 0x20A` / `sc` pattern for USB
   syscalls, we can hook at the LV2 level. Is this practical from a
   user-space SPRX?

3. **Can we use our own SPRX's resolved imports?** Our SPRX links
   `-lusbd_stub`, so CellOS resolves `cellUsbdInterruptReceive` in
   our own import table. Can we extract the resolved code address
   from our own OPD and search the game's memory for that specific
   address? This would tell us where the game's OPD table stores the
   same function pointer.

4. **PowerPC branch hook**: Assuming we find the target, what's the
   correct branch instruction format for PPC64? Standard `b` is
   ±32MB range — is that sufficient, or do we need the `ld r12, addr;
   mtctr r12; bctr` trampoline for longer jumps?

## Environment
- Game: LEGO Dimensions (BLES-02175), PID 0x1010200
- EBOOT.elf: 29.8MB, decrypted from EBOOT.BIN via oscetool -d
- Architecture: PPC64 ELFv1, OPD-based function calls
- SPRX: Sony DUPLEX SDK 3.40, ppu-lv2-gcc -mprx -O2
- Injection: PS3MAPI MODULE LOAD
