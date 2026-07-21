/* toc_trampoline.s — REFACTORED 2026-07-20 (Expert OPD Fix)
 * Architecture: PowerPC (CellOS Sony SDK -mprx)
 *
 * PURPOSE:
 *   Safe stack frame allocation, r2 (TOC) preservation, and TOC passing
 *   for userland hooks. Called from the 4-instruction preamble at each
 *   target function (cellUsbdInit, OpenPipe, Transfer, ClosePipe).
 *
 * OPD COMPLIANCE (via C):
 *   This file contains ONLY .text code. NO .opd sections.
 *   OPD entries are constructed dynamically in C (toc_trampoline_c.c).
 *   All symbols use "asm_" prefix to avoid OPD conflicts.
 *
 * CALLING CONVENTION:
 *   HOOK_WRAPPER: called from 4-insn preamble via bctr with bctr.
 *     - r1 = game's stack pointer
 *     - r2 = game's TOC
 *     - r3-r10 = game's original arguments
 *     - Returns to game's caller (saves LR, restores everything)
 *
 *   HOOK_PASSTHROUGH: called from C hooks as function pointer through OPD.
 *     - r3 = game_toc to restore
 *     - r4 = trampoline address (raw .text addr of saved insns)
 *     - r5+ = original game arguments shifted down
 *     - Returns to C caller
 */

/* ================================================================
 * MACRO: HOOK_WRAPPER
 *
 * Called from 4-instruction preamble. The preamble branches here
 * with the game's r2 (TOC) intact. We save it, call our C hook,
 * restore it, and return.
 *
 * Parameters:
 *   \asm_name    - Global label (asm_ prefix, no dot)
 *   \c_function  - C function to branch-and-link to
 *   \toc_reg     - Register to load TOC into (matching C signature)
 * ================================================================ */
.macro HOOK_WRAPPER asm_name, c_function, toc_reg
    .section .text
    .align 2
    .globl \asm_name
    \asm_name:
        /* 1. Allocate 0x60-byte stack frame */
        stwu %r1, -0x60(%r1)

        /* 2. Save Link Register (LR) into the frame */
        mflr %r0
        stw  %r0, 0x64(%r1)

        /* 3. Save the Game's TOC (r2) at offset 0x28 */
        stw  %r2, 0x28(%r1)

        /* 4. Load Game's TOC into the specified arg register */
        lwz  \toc_reg, 0x28(%r1)

        /* 5. Branch to C hook function (PRX TOC loaded by prologue) */
        bl   \c_function

        /* 6. Restore the Game's TOC (r2) */
        lwz  %r2, 0x28(%r1)

        /* 7. Restore Link Register (LR) */
        lwz  %r0, 0x64(%r1)
        mtlr %r0

        /* 8. Deallocate stack frame and return to game's caller */
        addi %r1, %r1, 0x60
        blr
.endm

/* ================================================================
 * MACRO: HOOK_PASSTHROUGH
 *
 * Called from C hooks via OPD function pointer.
 * Takes game_toc (r3) and tramp_addr (r4), restores game's TOC,
 * shifts arguments, calls trampoline, returns to C caller.
 *
 * Parameters:
 *   \asm_name   - Global label (asm_ prefix, no dot)
 *   \num_args   - Number of original game function arguments
 * ================================================================ */
.macro HOOK_PASSTHROUGH asm_name, num_args
    .section .text
    .align 2
    .globl \asm_name
    \asm_name:
        /* Allocate stack frame and save LR */
        stwu %r1, -0x60(%r1)
        mflr %r0
        stw  %r0, 0x64(%r1)

        /* Restore the Game's TOC (r2) from r3 */
        mr   %r2, %r3

        /* Save trampoline address from r4 into r11 */
        mr   %r11, %r4

        /* Shift original arguments down (r5+ -> r3+) */
        .if \num_args >= 1
        mr   %r3, %r5
        .endif
        .if \num_args >= 2
        mr   %r4, %r6
        .endif
        .if \num_args >= 3
        mr   %r5, %r7
        .endif
        .if \num_args >= 4
        mr   %r6, %r8
        .endif
        .if \num_args >= 5
        mr   %r7, %r9
        .endif
        .if \num_args >= 6
        mr   %r8, %r10
        .endif

        /* Branch to trampoline via count register */
        mtctr %r11
        bctrl

        /* Restore LR and deallocate stack frame */
        lwz  %r0, 0x64(%r1)
        mtlr %r0
        addi %r1, %r1, 0x60
        blr
.endm


/* ================================================================
 * HOOK WRAPPER INSTANTIATIONS (4 targets)
 * Called from 4-insn preamble via bctr (raw .text address)
 * ================================================================ */

/* cellUsbdInit has 0 args -> TOC must be passed in r3 */
HOOK_WRAPPER asm_wrapper_my_cellUsbdInit,      my_cellUsbdInit,      %r3

/* cellUsbdOpenPipe has 3 args (r3,r4,r5) -> TOC in r6 */
HOOK_WRAPPER asm_wrapper_my_cellUsbdOpenPipe,  my_cellUsbdOpenPipe,  %r6

/* cellUsbdTransfer has 5 args (r3,r4,r5,r6,r7) -> TOC in r8 */
HOOK_WRAPPER asm_wrapper_my_cellUsbdTransfer,  my_cellUsbdTransfer,  %r8

/* cellUsbdClosePipe has 1 arg (r3) -> TOC in r4 */
HOOK_WRAPPER asm_wrapper_my_cellUsbdClosePipe, my_cellUsbdClosePipe, %r4

/* ================================================================
 * PASSTHROUGH STUB INSTANTIATIONS (3 targets)
 * Called from C hooks through OPD function pointers
 * ================================================================ */

/* OpenPipe: 3 original args (pipe_handle, dev_id, ep_descriptor) */
HOOK_PASSTHROUGH asm_passthrough_OpenPipe,   3

/* Transfer: 5 original args (pipe, buf, len, arg4, arg5) */
HOOK_PASSTHROUGH asm_passthrough_Transfer,   5

/* ClosePipe: 1 original arg (pipe_handle) */
HOOK_PASSTHROUGH asm_passthrough_ClosePipe,  1
