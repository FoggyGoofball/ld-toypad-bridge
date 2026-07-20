# Expert Evaluation: LD-ToyPad Bridge Hook Architecture (2026-07-20)

## Purpose

This document catalogues architectural decisions made during the most recent
fix cycle targeting the five critical hooking defects. These questions are
posed to a PowerPC/CellOS expert to validate correctness and identify any
oversights before production deployment.

---

## 1. Four-Instruction Preamble Design (Defect 1 & 5)

### Decision

Replaced the out-of-range `ba` (Branch Absolute) instruction with a
four-instruction preamble that loads the hook target address into r11
and branches via the Count Register:

```
lis r11, <target_addr[31:16]>   # 0x3D60xxxx
ori r11, r11, <target_addr[15:0]> # 0x616Bxxxx
mtctr r11                        # 0x7D6B03A6
bctr                             # 0x4E800420
```

Implemented in `hook.c` → `hook_build_*()` functions.

### Questions

1. **Register volatility:** r11 is caller-save per the PowerPC EABI. Is
   there any scenario where the game's code between the `bctr` and the
   trampoline entry (`wrapper_my_cellUsbdInit`) could depend on r11
   retaining a value? (The trampoline does not save/restore r11.)

2. **Cache coherency:** The patch writes four words to the .text segment
   and then executes `ppu_dcbst` + `ppu_sync` + `ppu_icbi`. Is this
   sufficient for a running PPC executable, or is a `sync` + `isync`
   sequence also required on the executing core?

3. **Atomicity of 4-instruction patch:** If the game thread is executing
   through the target instruction stream while another core/thread is
   writing the patch, is there a risk of a partial patch being executed?
   Should the target thread be suspended (via PS3MAPI) during install?

4. **Return address:** The `bctr` does not set LR. The trampoline (inline
   asm in usb_hooks.c) saves LR on entry and restores it before `blr`.
   Is LR guaranteed to point to the instruction after the original
   call site in all hook contexts (cellUsbdInit, OpenPipe, Transfer,
   ClosePipe)?

---

## 2. Inline Asm TOC Trampoline vs. Separate .s File (Defect 2)

### Decision

Instead of a separate `toc_trampoline.s` assembly file (as the handoff
document specified), the TOC save/restore was implemented as inline
assembly macros (`TOC_SAVE`, `TOC_RESTORE`, `TOC_CALL_TRAMPOLINE`) in
`usb_hooks.c`. Each hook function is wrapped by a static trampoline.

### Questions

5. **GCC inline asm correctness for r2:** The `TOC_SAVE` macro uses
   `"stw %%r2, %0" : "=m"(saved_toc)`, and `TOC_RESTORE` uses
   `"lwz %%r2, %0" :: "m"(saved_toc)`. Is the `"m"` constraint
   sufficient to prevent GCC from optimizing away or reordering these
   stores/loads relative to the C function body?

6. **PRX TOC loading by compiler:** When GCC compiles the C function
   (e.g., `my_cellUsbdInit`), it automatically emits a prologue that
   loads the PRX's r2 from the PRX's own .got2 section. Is there any
   risk that the compiler's TOC load occurs *before* the inline asm
   TOC_SAVE executes, causing the saved r2 to already be the PRX's
   TOC rather than the game's TOC? (The save is at the very top of
   the wrapper, but the compiler may schedule instructions.)

7. **Separate .s file advantages:** The handoff document recommended a
   dedicated `toc_trampoline.s` with `HOOK_WRAPPER` macros. Does the
   separate file approach provide any correctness guarantees that inline
   asm cannot achieve, particularly regarding instruction scheduling
   and TOC pointer loading?

---

## 3. GOT Address Calculation in NID Scanner (Defect 3)

### Decision

`scan_for_nid()` in `usb_hooks.c` now emulates the `lis` instruction
to reconstruct the 32-bit GOT slot address before dereferencing with
`lwz`.

The original code attempted:
```c
addr = (nid_high << 16) + offset;  // WRONG
```

The fix:
```c
uint32_t lis_imm = (instruction & 0xFFFF);
uint32_t got_slot_base = (int16_t)(lis_imm << 16) + nid_scanner_addr;
// Then read: real_fn_ptr = *(uint32_t*)got_slot_base
```

### Questions

8. **Sign extension:** The `lis` instruction uses signed immediate
   (16-bit two's complement). The fix uses `(int16_t)(lis_imm << 16)`.
   Is this the correct way to sign-extend the 16-bit immediate to
   32 bits for the base address calculation?

9. **GOT slot dereference direction:** The `lwz r12, offset(r11)` loads
   from `r11 + offset`. Is the offset always positive (relative to the
   GOT base), or could it be negative for certain PRX module layouts?
   Should the code handle negative offsets?

10. **Multiple GOT sections:** A PRX can have both .got and .got2. Does
    the NID scan always target .got2 entries, or could some function
    pointers reside in .got? How does the scanner distinguish?

---

## 4. Passthrough TOC Trampoline (Defect 4)

### Decision

When a non-ToyPad USB device triggers the hook, the passthrough path
uses `TOC_CALL_TRAMPOLINE` inline asm:

```c
#define TOC_CALL_TRAMPOLINE(orig_fn, args...) \
    ({ void (*fn)() = (void (*)())orig_fn;    \
       uint32_t game_toc = g_game_toc;        \
       __asm__ volatile(                      \
           "lwz %%r2, %0\n\t"                \
           : : "m"(game_toc) : "memory");    \
       fn(args);                              \
       __asm__ volatile(                      \
           "stw %%r2, %0\n\t"                \
           "lwz %%r2, %1\n\t"                \
           : "=m"(game_toc), "m"(prx_toc_saved) : ); })
```

### Questions

11. **r2 clobber declaration:** The inline asm for restoring the game's
    TOC uses `"m"(game_toc)` as input, but does not list r2 as a
    clobbered register. Is GCC guaranteed to emit the correct r2
    restoration after this asm block, or could the compiler re-assert
    the PRX TOC immediately after the asm block when the function
    continues executing?

12. **Function pointer call after TOC restore:** After `"lwz %%r2, %0"`
    loads the game's TOC, the C code calls `fn(args)`. At this point,
    r2 holds the game's TOC. The original cellUsbd function expects
    the game's TOC. Is there any scenario where the original function
    itself modifies r2 internally (e.g., calling into another PRX
    module that resets r2) and we'd need to re-restore the game's TOC
    after the call returns?

13. **PRX TOC re-load after passthrough call returns:** After `fn(args)`
    returns, the inline asm stores the (potentially updated) r2 back to
    `game_toc` and loads `prx_toc_saved` (the PRX TOC saved at function
    entry). Is the store to `game_toc` necessary, or should we always
    re-load the originally saved game TOC value from the stack?

---

## 5. Memory Protection Bypass

### Decision

The handoff mandates installing the 4-instruction patch via
`sys_process_write_memory` (LV2 syscall) to bypass the R-X page
protection on the .text segment. However, the current implementation
writes the patch directly via PS3MAPI's memory write endpoint
(`/cpursx.ps3?/write_process...`), which the Node.js injector uses.

### Questions

14. **PS3MAPI vs. raw syscall:** Does the PS3MAPI `/write_process`
    endpoint internally perform the equivalent of
    `sys_process_write_memory`, or does it use standard memory write
    which would trigger a DSI (Data Storage Interrupt) on R-X pages?

15. **Who installs the patch?** Currently, the patch installation
    function (`hook_install` in `hook.c`) runs *inside* the SPRX
    after injection. The SPRX has kernel-level access since it's
    loaded as an LV2 PRX. Is the `sys_process_write_memory` syscall
    needed at all when the patching code already runs with kernel
    privileges inside the target process context?

16. **Fallback strategy:** If PS3MAPI's memory write does trigger a DSI
    on .text pages, should the patch installation be done from the
    Node.js injector script (which has no such page restrictions when
    writing via PS3MAPI) rather than from within the SPRX?

---

## 6. Build System & Toolchain

### Decision

The SPRX is compiled with the Sony SDK GCC (`ppu-lv2-gcc.exe`) on
Windows, linked with `-mprx -nodefaultlibs -llv2_stub -lfs_stub
-lnet_stub`, and signed with `oscetool` from WSL.

### Questions

17. **Stub libraries:** The link flags include `-llv2_stub`, `-lfs_stub`,
    and `-lnet_stub`. Are these the correct stub libraries for the
    syscalls used (particularly LV2 syscalls like
    `sys_process_write_memory` if implemented), or do we need
    additional stub libraries?

18. **oscetool flags:** The signing uses `-0 SELF -1 TRUE -2 000A
    -3 1010000001000003 ...`. Is the type `SELF` with application type
    `APP` correct for a game-mode SPRX plugin loaded via
    `load_prx`/PS3MAPI, or should it be `-0 SPRX` with type `NP`?

19. **Section alignment:** The linker output is 97,360 bytes raw PRX.
    The signed SELF is 16,656 bytes. Does this compression ratio
    (roughly 6:1) seem normal for oscetool with `-e` (encrypt), or
    could sections be getting stripped or misaligned?

---

*Prepared by Agent on 2026-07-20 for expert review.*
