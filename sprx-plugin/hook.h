/**
 * hook.h — PowerPC User-Space Detour Hooking (Sony SDK)
 *
 * Provides user-space trampoline hooks for intercepting game function calls.
 * Designed for LEGO Dimensions Toy Pad emulation — hooks cellUsbd functions
 * inside the game process after PRX injection via PS3MAPI.
 *
 * Architecture:
 *   - Overwrites first 4 instructions of target function with an absolute
 *     branch (ba opcode) to our hook function.
 *   - Saves original instructions in a trampoline to call-through for
 *     non-Toy-Pad USB traffic (controllers, storage, etc.).
 *   - Handles PowerPC TOC (r2) switching between game and PRX contexts.
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

/** Number of instructions to patch (4 = 16 bytes) */
#define HOOK_NUM_INSTRUCTIONS  4

/** Size of trampoline in bytes (4 saved + 1 branch-back = 5 instructions = 20 bytes) */
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
    uint32_t branch_back;                           /**< ba opcode back to target+16 */
} hook_trampoline_t;

/**
 * Hook context passed to the preamble generator.
 * Contains all info needed to build the TOC-switching entry stub.
 */
typedef struct {
    void *target_addr;       /**< Address being hooked (game function) */
    void *hook_fn;           /**< Address of our replacement function */
    void *prx_toc;           /**< Our PRX's TOC base (from __toc) */
    hook_trampoline_t *tramp;/**< Allocated trampoline buffer */
} hook_ctx_t;

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

/**
 * Install a detour hook on a target function.
 *
 * 1. Saves first 4 instructions (16 bytes) of target into trampoline.
 * 2. Builds branch-back at trampoline[4] (ba target+16).
 * 3. Writes 4 NOPs at target (safe window for icache sync).
 * 4. Overwrites target with absolute branch to hook_fn.
 * 5. Flushes instruction cache for both target and trampoline.
 *
 * @param ctx     Fully populated hook context (target, hook, toc, tramp).
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
 * Call the original function through the trampoline.
 *
 * Used by hooks that want to pass through non-Toy-Pad traffic.
 * This function handles TOC switching and jumps to the trampoline.
 *
 * NOTE: For a variable-argument hook like cellUsbdTransfer, the caller
 * should use inline assembly or a direct function-pointer call through
 * the trampoline. This helper is for fixed-signature cases.
 *
 * @param tramp   Trampoline containing original instructions.
 * @param args    Pointer to argument struct (varies per function).
 * @return        Return value from original function.
 */
int hook_call_original(hook_trampoline_t *tramp, void *args);

/**
 * Flush the instruction cache for a memory range.
 * Required after writing executable code (trampoline or patched function).
 *
 * @param addr   Start address (must be cache-line aligned).
 * @param size   Size in bytes.
 */
void hook_flush_icache(void *addr, uint32_t size);

/**
 * Build a PowerPC ba (Branch Absolute) opcode.
 *
 * PowerPC ba instruction format:
 *   Bits 0-5:   0x12 (OPCD)
 *   Bits 6-29:  LI  (absolute target address >> 2, masked to 24 bits)
 *   Bits 30-31: 0x02 (AA=1, LK=0)
 *
 * AA=1 means absolute address (not relative).
 *
 * @param target  Target address (must be 4-byte aligned).
 * @return        The 32-bit ba opcode.
 */
static inline uint32_t hook_build_ba(void *target)
{
    uint32_t addr = (uint32_t)(uintptr_t)target;
    /* ba: 0x48000002 | (addr & 0x03FFFFFC) */
    return 0x48000002 | (addr & 0x03FFFFFC);
}

/**
 * Build a PowerPC nop (ori r0, r0, 0) opcode.
 */
static inline uint32_t hook_build_nop(void)
{
    return 0x60000000;
}

#ifdef __cplusplus
}
#endif

#endif /* HOOK_H */
