/**
 * hook.c — PowerPC User-Space Detour Hooking Engine (Sony SDK)
 *
 * Implements the detour hook engine for intercepting game function calls.
 *
 * Hook strategy:
 *   1. Read 4 instructions (16 bytes) from target function.
 *   2. Write them into a trampoline buffer, followed by a ba opcode
 *      that branches back to target+16 (the instruction after our patch).
 *   3. Overwrite the 4 instructions at target with our injected preamble:
 *        - mflr r0         (save link register)
 *        - stw  r0, 4(r1)  (push lr onto stack)
 *        - stw  r2, 8(r1)  (save game's TOC)
 *        - ba   hook_fn    (branch absolute to our hook)
 *   4. Our hook function entry does:
 *        - Restore game TOC from stack
 *        - Restore game LR from stack
 *        - Execute hook logic (route to network.c)
 *        - For passthrough: bl  trampoline  (call original via trampoline)
 *        - Return with blr
 *   5. Flush icache for both patched region and trampoline.
 *
 * IMPORTANT: The 4-instruction preamble is injected INLINE at the target,
 * not called via a stub. This means the hook function itself must be
 * written as a regular C function that the game calls directly.
 * The TOC switching happens in the preamble.
 *
 * Reference: Standard PowerPC function call convention
 *   r2  = TOC pointer (must be preserved across calls)
 *   r3+ = function arguments
 *   LR  = return address
 */

#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <sys/timer.h>  /* for __sync_synchronize if available */

#include "hook.h"
#include "debug.h"

/* ---------------------------------------------------------------
 * PowerPC Cache-Control Intrinsics
 * --------------------------------------------------------------- */

/*
 * __dcbst — Data Cache Block Store (flush to memory).
 * __sync  — Synchronize (wait for previous stores to complete).
 * __icbi — Instruction Cache Block Invalidate.
 * __isync — Instruction Synchronize (wait for icache to refill).
 *
 * These are implemented as inline assembly macros to avoid relying on
 * GCC builtins that may not be available in the Sony SDK toolchain.
 */

/** Flush a data cache line (write back to memory). */
static inline void ppu_dcbst(void *addr)
{
    __asm__ volatile (
        "dcbst 0, %0\n\t"
        "sync\n\t"
        "icbi 0, %0\n\t"
        "isync\n\t"
        :
        : "r"(addr)
        : "memory"
    );
}

/** Instruction sync barrier. */
static inline void ppu_sync(void)
{
    __asm__ volatile ("sync" ::: "memory");
}

/** Instruction cache block invalidate. */
static inline void ppu_icbi(void *addr)
{
    __asm__ volatile ("icbi 0, %0" :: "r"(addr) : "memory");
}

/** Instruction synchronize (drain icache pipeline). */
static inline void ppu_isync(void)
{
    __asm__ volatile ("isync");
}

/* ---------------------------------------------------------------
 * Implementation
 * --------------------------------------------------------------- */

void hook_flush_icache(void *addr, uint32_t size)
{
    uint32_t i;

    /* Align address to cache line boundary (128 bytes on PPU). */
    uintptr_t aligned = (uintptr_t)addr & ~(uintptr_t)0x7F;
    uintptr_t end = (uintptr_t)addr + (uintptr_t)size;

    /* Flush each cache line in range. */
    for (i = 0; aligned < end; aligned += 128, i++) {
        ppu_dcbst((void *)aligned);
    }

    ppu_sync();

    /* Invalidate icache for the same range. */
    aligned = (uintptr_t)addr & ~(uintptr_t)0x7F;
    for (i = 0; aligned < end; aligned += 128, i++) {
        ppu_icbi((void *)aligned);
    }

    ppu_isync();
}

int hook_install(hook_ctx_t *ctx)
{
    uint32_t *target;
    uint32_t *tramp_insns;
    int i;

    if (ctx == NULL || ctx->target_addr == NULL || ctx->hook_fn == NULL ||
        ctx->tramp == NULL) {
        DEBUG_ERROR("[HOOK] hook_install: invalid args\n");
        return -1;
    }

    target = (uint32_t *)ctx->target_addr;
    tramp_insns = ctx->tramp->instructions;

    /* ---------------------------------------------------------------
     * Step 1: Save original 4 instructions from target into trampoline.
     * --------------------------------------------------------------- */
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++) {
        tramp_insns[i] = target[i];
    }

    /* ---------------------------------------------------------------
     * Step 2: Build branch-back at trampoline[4] (ba target+16).
     * --------------------------------------------------------------- */
    ctx->tramp->branch_back = hook_build_ba(
        (void *)((uintptr_t)ctx->target_addr + (HOOK_NUM_INSTRUCTIONS * 4)));

    /* ---------------------------------------------------------------
     * Step 3: Write 4 NOPs at target as a safe window.
     *         This prevents partially-patched code from executing.
     * --------------------------------------------------------------- */
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++) {
        target[i] = hook_build_nop();
    }
    hook_flush_icache(ctx->target_addr, HOOK_NUM_INSTRUCTIONS * 4);

    /* ---------------------------------------------------------------
     * Step 4: Overwrite target[0] with absolute branch to hook_fn.
     *         We use a single ba instruction that replaces all 4 slots.
     *         The remaining 3 NOPs serve as padding for the trampoline's
     *         branch-back target.
     *
     *         This is the simplest possible hook: a 4-instruction preamble
     *         that immediately jumps to our replacement function.
     *
     *         Our replacement function must be written with the SAME
     *         calling convention as the original. Since the game's
     *         cellUsbd imports are standard PPC calls (r3=arg1, r4=arg2...),
     *         our hook function receives the exact same arguments.
     * --------------------------------------------------------------- */
    target[0] = hook_build_ba(ctx->hook_fn);
    /* target[1..3] remain NOPs from step 3 */

    /* ---------------------------------------------------------------
     * Step 5: Flush icache for both target and trampoline.
     * --------------------------------------------------------------- */
    hook_flush_icache(ctx->target_addr, HOOK_NUM_INSTRUCTIONS * 4);
    hook_flush_icache(ctx->tramp, HOOK_TRAMPOLINE_SIZE);

    DEBUG_PRINT("[HOOK] Installed: target=0x%08X -> hook=0x%08X\n",
                (unsigned)(uintptr_t)ctx->target_addr,
                (unsigned)(uintptr_t)ctx->hook_fn);
    DEBUG_PRINT("[HOOK] Trampoline at 0x%08X, branch-back to 0x%08X\n",
                (unsigned)(uintptr_t)ctx->tramp,
                (unsigned)(uintptr_t)ctx->target_addr + (HOOK_NUM_INSTRUCTIONS * 4));

    return 0;
}

int hook_remove(hook_ctx_t *ctx)
{
    uint32_t *target;
    int i;

    if (ctx == NULL || ctx->target_addr == NULL || ctx->tramp == NULL) {
        DEBUG_ERROR("[HOOK] hook_remove: invalid args\n");
        return -1;
    }

    target = (uint32_t *)ctx->target_addr;

    /* Write 4 NOPs first (safe window). */
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++) {
        target[i] = hook_build_nop();
    }
    hook_flush_icache(ctx->target_addr, HOOK_NUM_INSTRUCTIONS * 4);

    /* Restore original instructions from trampoline. */
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++) {
        target[i] = ctx->tramp->instructions[i];
    }
    hook_flush_icache(ctx->target_addr, HOOK_NUM_INSTRUCTIONS * 4);

    DEBUG_PRINT("[HOOK] Removed: restored target at 0x%08X\n",
                (unsigned)(uintptr_t)ctx->target_addr);

    return 0;
}

int hook_call_original(hook_trampoline_t *tramp, void *args)
{
    /*
     * NOTE: This is a placeholder for generic trampoline calls.
     * For cellUsbd functions, the hook functions use inline assembly
     * to call through the trampoline with proper TOC handling.
     *
     * This function is provided for cases where the hook doesn't need
     * to modify arguments.
     */
    (void)tramp;
    (void)args;
    DEBUG_ERROR("[HOOK] hook_call_original: generic stub not implemented\n");
    return -1;
}
