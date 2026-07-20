/**
 * hook.h — PowerPC User-Space Detour Hooking (Sony SDK)
 *
 * FIXED: Replaced ba-only preamble (limited to +-32MB) with lis/ori/mtctr/bctr
 * that can reach any 32-bit address. Added proper TOC (r2) save/restore.
 *
 * Architecture:
 *   - Overwrites first 4 instructions of target function with a preamble
 *     that saves LR, saves TOC, and loads the hook address into r11.
 *   - Writes mtctr/bctr at target+16 (2 additional instructions beyond
 *     the 4-instruction patch window).
 *   - Saves original instructions in a trampoline to call-through for
 *     non-Toy-Pad USB traffic (controllers, storage, etc.).
 *   - TOC switching is handled via the preamble saving r2 to the stack;
 *     hook functions restore game's r2 before passthrough calls.
 *
 * Requires: Sony SDK -mprx, -llv2_stub (for __sync_synchronize etc.)
 *           Running inside target game process (user-space).
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

/** Number of instructions to patch in the target preamble (4 = 16 bytes) */
#define HOOK_NUM_INSTRUCTIONS  4

/**
 * Total size of our patch: 4 preamble instructions + 2 extension
 * (mtctr/bctr at target+16) = 24 bytes.
 * The trampoline saves only the original HOOK_NUM_INSTRUCTIONS = 4.
 * The branch-back from trampoline goes to target+24 (after entire patch).
 */
#define HOOK_PATCH_TOTAL_SIZE   (6 * 4)

/** Size of trampoline in bytes (4 saved + 1 branch-back = 20 bytes) */
#define HOOK_TRAMPOLINE_SIZE   (5 * 4)

/* ---------------------------------------------------------------
 * Types
 * --------------------------------------------------------------- */

/**
 * Trampoline buffer — holds original instructions + branch-back.
 * Must be executable (allocate in .text or use memalign + mprotect).
 */
typedef struct {
    uint32_t instructions[HOOK_NUM_INSTRUCTIONS];  /**< Original first 4 insns */
    uint32_t branch_back;                           /**< ba opcode back to target+24 */
} hook_trampoline_t;

/**
 * Hook context passed to the preamble generator.
 * Contains all info needed to build the detour patch.
 */
typedef struct {
    void *target_addr;       /**< Address being hooked (game function) */
    void *hook_fn;           /**< Address of our replacement function */
    hook_trampoline_t *tramp;/**< Allocated trampoline buffer */
} hook_ctx_t;

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

/**
 * Install a detour hook on a target function.
 *
 * 1. Saves first 4 instructions (16 bytes) of target into trampoline.
 * 2. Builds branch-back at trampoline[4] (ba target+24).
 * 3. Writes 4 NOPs at target (safe window for icache sync).
 * 4. Overwrites target[0..3] with preamble:
 *      [0] mflr r0          (save link register)
 *      [1] stw  r2, 8(r1)   (save game TOC on stack)
 *      [2] lis  r11, hi     (load high 16 bits of hook addr)
 *      [3] ori  r11, lo     (load low 16 bits)
 * 5. Writes mtctr/bctr at target+16 (patch extension).
 * 6. Flushes instruction cache for both target and trampoline.
 *
 * @param ctx     Fully populated hook context (target, hook, tramp).
 * @return        0 on success, negative on error.
 */
int hook_install(hook_ctx_t *ctx);

/**
 * Remove a detour hook and restore original instructions.
 *
 * @param ctx     Hook context with target and trampoline.
 * @return        0 on success, negative on error.
 */
int hook_remove(hook_ctx_t *ctx);

/**
 * Flush the instruction cache for a memory range.
 * Required after writing executable code (trampoline or patched function).
 *
 * @param addr   Start address.
 * @param size   Size in bytes.
 */
void hook_flush_icache(void *addr, uint32_t size);

/* ---------------------------------------------------------------
 * PowerPC Opcode Builders
 * --------------------------------------------------------------- */

/** nop: ori r0, r0, 0 */
static inline uint32_t hook_build_nop(void)
{
    return 0x60000000;
}

/** mflr r0 — move link register to r0 */
static inline uint32_t hook_build_mflr_r0(void)
{
    return 0x7C0802A6;
}

/** stw r2, 8(r1) — save TOC to stack */
static inline uint32_t hook_build_stw_r2(void)
{
    return 0x90410008;
}

/** lwz r2, 8(r1) — restore TOC from stack */
static inline uint32_t hook_build_lwz_r2(void)
{
    return 0x80410008;
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

/**
 * Build a PowerPC ba (Branch Absolute) opcode.
 * Used for the trampoline branch-back.
 *
 * ba format:
 *   Bits 0-5:   0x12 (OPCD)
 *   Bits 6-29:  LI  = address >> 2 (24-bit)
 *   Bits 30-31: 0x02 (AA=1, LK=0)
 *
 * @param target  Target address (must be 4-byte aligned).
 * @return        The 32-bit ba opcode.
 */
static inline uint32_t hook_build_ba(void *target)
{
    uint32_t addr = (uint32_t)(uintptr_t)target;
    /* LI field must be addr / 4, masked to 24 bits */
    return 0x48000002 | ((addr >> 2) & 0x03FFFFFC);
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

#ifdef __cplusplus
}
#endif

#endif /* HOOK_H */
