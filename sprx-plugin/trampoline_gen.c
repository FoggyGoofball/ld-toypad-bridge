/**
 * trampoline_gen.c — Dynamic PowerPC Trampoline Generator
 *
 * Generates 64-byte (16 instruction) R-W-X trampolines at runtime
 * that save/restore the game's TOC (r2), pass it as an argument to
 * a C hook function, load the SPRX's TOC, and call the hook via
 * branch-and-link.
 *
 * WHY DYNAMIC:
 *   The PS3's 16-byte PLT stub does not dereference OPDs. It loads
 *   a raw code pointer from the GOT slot and branches to it via
 *   lwz r12, offset(r11); mtctr r12; bctr. Writing an OPD pointer
 *   into the GOT causes bctr to jump to the OPD's data bytes as
 *   instructions, triggering an Instruction Storage Interrupt (ISI).
 *
 *   Dynamic trampolines solve this by being actual executable code
 *   that properly saves/restores the game's TOC and calls our C
 *   hook with the correct ABI.
 *
 * ARCHITECTURE (16 instructions / 64 bytes):
 *   [0]  stwu  r1, -0x60(r1)          — Allocate stack frame
 *   [1]  mflr  r0                     — Save LR
 *   [2]  stw   r0, 0x64(r1)          — Store LR in frame
 *   [3]  stw   r2, 0x28(r1)          — Save game's TOC
 *   [4]  mr    toc_arg_reg, r2       — Pass game TOC as argument
 *   [5]  lis   r2, toc_hi            — Load SPRX TOC high
 *   [6]  ori   r2, r2, toc_lo        — Load SPRX TOC low
 *   [7]  lis   r12, code_hi          — Load C function code addr high
 *   [8]  ori   r12, r12, code_lo     — Load C function code addr low
 *   [9]  mtctr r12                   — Move to count register
 *   [10] bctrl                       — Branch-and-link to C hook
 *   [11] lwz   r2, 0x28(r1)         — Restore game's TOC
 *   [12] lwz   r0, 0x64(r1)         — Restore LR
 *   [13] mtlr  r0                    — Move LR back
 *   [14] addi  r1, r1, 0x60         — Deallocate stack frame
 *   [15] blr                         — Return to game
 *
 * (c) 2026 LD-ToyPad Bridge Team
 */

#include <stdint.h>
#include "trampoline_gen.h"

/**
 * CellOS OPD (Official Procedure Descriptor) structure.
 * On PowerPC 32-bit CellOS, function pointers point to a 12-byte OPD
 * struct containing: code address, TOC address, environment pointer.
 */
typedef struct {
    uint32_t code_addr;    /* Ptr to .text code */
    uint32_t toc_addr;     /* TOC base value (loaded into r2 on function entry) */
    uint32_t env_ptr;      /* Environment pointer (unused, set to 0) */
} ppc_opd_t;

void create_hook_trampoline(uint32_t *tramp, void *c_func, int toc_arg_reg)
{
    const ppc_opd_t *opd = (const ppc_opd_t *)c_func;
    uint32_t code = opd->code_addr;
    uint32_t toc  = opd->toc_addr;
    int i = 0;

    /* ---------------------------------------------------------------
     * Standard PowerPC stack frame prologue (32 bytes of save area)
     * --------------------------------------------------------------- */

    /* [0] stwu r1, -0x60(r1) — Allocate 0x60-byte stack frame */
    tramp[i++] = 0x9421FFA0u;

    /* [1] mflr r0 — Save link register into r0 */
    tramp[i++] = 0x7C0802A6u;

    /* [2] stw r0, 0x64(r1) — Store LR at saved LR slot */
    tramp[i++] = 0x90010064u;

    /* [3] stw r2, 0x28(r1) — Save game's TOC at offset 0x28 */
    tramp[i++] = 0x90410028u;

    /* ---------------------------------------------------------------
     * Pass game's TOC to C hook as an argument
     *
     * mr toc_arg_reg, r2 → or RA=toc_arg_reg, RS=r2, RB=r2
     * Encoding: 0x7C401378 | (toc_arg_reg << 16)
     *
     * CORRECTION NOTE:
     *   Originally encoded as:
     *     0x7C000378 | (toc_arg_reg << 21) | (2 << 16) | (2 << 11)
     *   This was WRONG — it encoded or r2, toc_arg_reg, r2 (mr r2, toc_arg_reg)
     *   which would destroy the game's TOC by overwriting r2 with whatever
     *   garbage was in the argument register.
     *
     *   The CORRECT encoding is:
     *     mr RA_dst, RS_src → or RA=RS_src, RS=RA_dst, RB=RS_src
     *     For mr toc_arg_reg, r2: RS=r2(2), RA=toc_arg_reg, RB=r2(2)
     *     Encoded: 0x7C000378 | (2 << 21) | (toc_arg_reg << 16) | (2 << 11)
     *     Simplified: 0x7C401378 | (toc_arg_reg << 16)
     * --------------------------------------------------------------- */

    /* [4] mr toc_arg_reg, r2 */
    tramp[i++] = 0x7C401378u | ((uint32_t)(toc_arg_reg & 0x1F) << 16);

    /* ---------------------------------------------------------------
     * Load SPRX's TOC into r2
     *
     * Before calling the C hook, we must restore the SPRX's own TOC
     * because the C compiler expects r2 to point to the SPRX's GOT.
     * The game's TOC was saved at step [3] and will be restored after
     * the C hook returns.
     * --------------------------------------------------------------- */

    /* [5] lis r2, toc@h */
    tramp[i++] = 0x3C400000u | ((toc >> 16) & 0xFFFF);

    /* [6] ori r2, r2, toc@l */
    tramp[i++] = 0x60420000u | (toc & 0xFFFF);

    /* ---------------------------------------------------------------
     * Load C function code address into r12 and call it
     * --------------------------------------------------------------- */

    /* [7] lis r12, code@h */
    tramp[i++] = 0x3D800000u | ((code >> 16) & 0xFFFF);

    /* [8] ori r12, r12, code@l */
    tramp[i++] = 0x618C0000u | (code & 0xFFFF);

    /* [9] mtctr r12 */
    tramp[i++] = 0x7D8903A6u;

    /* [10] bctrl — Branch to C hook (saves return addr in LR) */
    tramp[i++] = 0x4E800421u;

    /* ---------------------------------------------------------------
     * Epilogue: Restore game's TOC and return to game caller
     * --------------------------------------------------------------- */

    /* [11] lwz r2, 0x28(r1) — Restore game's TOC */
    tramp[i++] = 0x80410028u;

    /* [12] lwz r0, 0x64(r1) — Restore LR value */
    tramp[i++] = 0x80010064u;

    /* [13] mtlr r0 */
    tramp[i++] = 0x7C0803A6u;

    /* [14] addi r1, r1, 0x60 — Deallocate stack frame */
    tramp[i++] = 0x38210060u;

    /* [15] blr — Return to game */
    tramp[i++] = 0x4E800020u;

    /* ---------------------------------------------------------------
     * Flush Data & Instruction Caches
     *
     * CRITICAL: The PPU has a split L1 cache (data + instruction).
     * We wrote the trampoline instructions via data writes (stw),
     * so they reside in the data cache. The instruction cache still
     * has stale data at these addresses. Without flushing, the PPU
     * will fetch stale (0x00000000) instructions and crash.
     *
     * For each instruction word we wrote:
     *   dcbst  — Flush data cache line to L2 (write-back)
     *   sync   — Ensure dcbst completes
     *   icbi   — Invalidate instruction cache line
     *   isync  — Wait for icbi to complete
     *
     * WARNING: The trampoline is 16 instructions (64 bytes).
     * PowerPC cache lines are 128 bytes. We only need to flush
     * one 128-byte cache line, but flushing per-instruction is
     * safer in case of partial cache line alignment.
     * --------------------------------------------------------------- */

    for (int j = 0; j < i; j++) {
        __asm__ __volatile__ (
            "dcbst 0, %0\n\t"
            "sync\n\t"
            "icbi 0, %0\n\t"
            "isync"
            :: "r"(&tramp[j]) : "memory"
        );
    }
}
