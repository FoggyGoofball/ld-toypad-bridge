/**
 * hook.h — PowerPC Opcode Builders for SPRX Detour Hooks
 *
 * REFACTORED 2026-07-20:
 *   - Removed hook_install(), hook_remove(), hook_ctx_t, hook_trampoline_t
 *   - Removed hook_call_original()
 *   - Removed hook_flush_icache()
 *   - Retained ONLY the PowerPC opcode builder functions
 *
 * WHY:
 *   Preamble writing to the game's R-X .text segment now happens from
 *   Ring 0 via PS3MAPI HTTP endpoints (inject-sprx.js). The SPRX no
 *   longer directly writes to game memory. It allocates executable
 *   trampoline pages via sys_memory_allocate and communicates the
 *   addresses to the Node.js orchestrator via HDD IPC files.
 *
 * These opcode builders are used BOTH by:
 *   1. The SPRX (when preparing trampoline branch-back code)
 *   2. inject-sprx.js (when constructing the preamble via opcode constants)
 *
 * Architecture:
 *   Preamble (4 instructions, written by Node.js to game .text):
 *     [0] lis  r11, hi16(hook_wrapper_addr)
 *     [1] ori  r11, lo16(hook_wrapper_addr)
 *     [2] mtctr r11
 *     [3] bctr
 *
 *   Trampoline branch-back (8 instructions in executable page):
 *     [0-3] Original 4 instructions from target function
 *     [4]   lis  r11, hi16(target_addr + 16)
 *     [5]   ori  r11, lo16(target_addr + 16)
 *     [6]   mtctr r11
 *     [7]   bctr
 *
 * Requires: Sony SDK -mprx, -llv2_stub
 *           Running inside target game process (user-space).
 *
 * (c) 2026 LD-ToyPad Bridge Team
 */
#ifndef HOOK_H
#define HOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------- */

/** Number of instructions in the hook preamble. */
#define HOOK_NUM_INSTRUCTIONS  4

/** Size of trampoline (4 saved + 4 branch-back = 8 instructions = 32 bytes). */
#define HOOK_TRAMPOLINE_SIZE   (8 * 4)

/** Offset in trampoline's stack frame where game's TOC (r2) is saved.
 *  The assembly trampoline (toc_trampoline.s) saves r2 at 0x28(%r1)
 *  before branching to the C hook. */
#define HOOK_TOC_SAVE_OFFSET   0x28

/* ---------------------------------------------------------------
 * PowerPC Opcode Builders
 *
 * These are used by the SPRX to construct trampoline instructions,
 * AND their numeric outputs are replicated in inject-sprx.js for
 * preamble construction.
 * --------------------------------------------------------------- */

/** nop: ori r0, r0, 0 */
static inline uint32_t hook_build_nop(void)
{
    return 0x60000000;
}

/**
 * Build a PowerPC lis (Load Immediate Shifted) opcode.
 * lis rD, SIMM: rD = SIMM << 16
 * Format: 0x3C000000 | (rD << 21) | (SIMM & 0xFFFF)
 * For r11 (register 11): 0x3D600000 | (SIMM & 0xFFFF)
 */
static inline uint32_t hook_build_lis_r11(uint32_t val)
{
    return 0x3D600000 | (val & 0xFFFF);
}

/**
 * Build a PowerPC ori (OR Immediate) opcode.
 * ori rA, rS, UIMM: rA = rS | UIMM
 * For r11, r11: 0x616B0000 | (UIMM & 0xFFFF)
 */
static inline uint32_t hook_build_ori_r11(uint32_t val)
{
    return 0x616B0000 | (val & 0xFFFF);
}

/** mtctr r11 — move r11 to count register */
static inline uint32_t hook_build_mtctr_r11(void)
{
    return 0x7D6B03A6;
}

/** bctr — branch to count register (AA=0, LK=0) */
static inline uint32_t hook_build_bctr(void)
{
    return 0x4E800420;
}

/** bctrl — branch to count register with link (AA=0, LK=1) */
static inline uint32_t hook_build_bctrl(void)
{
    return 0x4E800421;
}

/**
 * Convenience: load a 32-bit value into r11 using lis/ori.
 * Writes two opcodes into out[0] and out[1].
 */
static inline void hook_build_load_r11(uint32_t val, uint32_t out[2])
{
    out[0] = hook_build_lis_r11(val >> 16);
    out[1] = hook_build_ori_r11(val & 0xFFFF);
}

/**
 * Build a complete trampoline in a buffer: 4 original instructions
 * followed by a 4-instruction absolute branch-back to target+16.
 *
 * @param tramp_buffer  8-word (32-byte) buffer to write into.
 *                      Must be in executable memory.
 * @param orig_insns    Pointer to the 4 original instructions at target.
 * @param target_addr   Address of the hooked function (preamble written
 *                      by Node.js). target+16 is the branch-back target.
 */
static inline void hook_build_trampoline(
    uint32_t *tramp_buffer,
    const uint32_t *orig_insns,
    uint32_t target_addr)
{
    int i;

    /* Copy original 4 instructions from target function */
    for (i = 0; i < 4; i++) {
        tramp_buffer[i] = orig_insns[i];
    }

    /* Build absolute branch-back to target+16 using lis/ori/mtctr/bctr.
     * This guarantees full 32-bit reach (unlike ba which is limited to
     * 0x00000000-0x01FFFFFF with a 24-bit absolute address). */
    {
        uint32_t back_addr = target_addr + 16;  /* past the 4-insn preamble */
        tramp_buffer[4] = hook_build_lis_r11(back_addr >> 16);
        tramp_buffer[5] = hook_build_ori_r11(back_addr & 0xFFFF);
        tramp_buffer[6] = hook_build_mtctr_r11();
        tramp_buffer[7] = hook_build_bctr();
    }
}

#ifdef __cplusplus
}
#endif

#endif /* HOOK_H */
