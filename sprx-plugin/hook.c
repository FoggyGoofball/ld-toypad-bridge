/**
 * hook.c — PowerPC User-Space Detour Hooking Engine (Sony SDK)
 *
 * FIXED: Replaced ba-only approach (limited to +-32MB) with lis/ori/mtctr/bctr
 * preamble that loads ANY 32-bit address. Added TOC save/restore.
 *
 * Preamble at target[0..3]:
 *   [0] mflr r0          (save link register)
 *   [1] stw  r2, 8(r1)   (save game TOC onto stack)
 *   [2] lis  r11, hi     (load high 16 bits of hook addr)
 *   [3] ori  r11, r11, lo (load low 16 bits)
 * Then at target[4..5]:
 *   [4] mtctr r11
 *   [5] bctr
 *
 * Trampoline: saves original 4 instructions from target.
 * Branch-back at trampoline[4] goes to target+24 (after the full 6-insn patch).
 *
 * TOC management:
 *   The preamble saves the game's r2 (TOC) to the stack at 8(r1).
 *   The trampoline function restores the game's r2 before each passthrough call
 *   by reading it from the call chain's backchain.
 */
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <sys/timer.h>

#include "hook.h"
#include "debug.h"

/* ---------------------------------------------------------------
 * PowerPC Cache-Control Intrinsics
 * --------------------------------------------------------------- */

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

static inline void ppu_sync(void)
{
    __asm__ volatile ("sync" ::: "memory");
}

static inline void ppu_icbi(void *addr)
{
    __asm__ volatile ("icbi 0, %0" :: "r"(addr) : "memory");
}

static inline void ppu_isync(void)
{
    __asm__ volatile ("isync");
}

/* ---------------------------------------------------------------
 * Cache flush
 * --------------------------------------------------------------- */

void hook_flush_icache(void *addr, uint32_t size)
{
    uint32_t i;

    /* Align address to PPU cache line boundary (128 bytes). */
    uintptr_t aligned = (uintptr_t)addr & ~(uintptr_t)0x7F;
    uintptr_t end = (uintptr_t)addr + (uintptr_t)size;

    /* Flush each data cache line in range. */
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

/* ---------------------------------------------------------------
 * Hook installation
 * --------------------------------------------------------------- */

int hook_install(hook_ctx_t *ctx)
{
    uint32_t *target;
    uint32_t hook_addr;
    uint32_t back_addr;
    int i;

    if (ctx == NULL || ctx->target_addr == NULL || ctx->hook_fn == NULL ||
        ctx->tramp == NULL) {
        DEBUG_ERROR("[HOOK] hook_install: invalid args\n");
        return -1;
    }

    target = (uint32_t *)ctx->target_addr;
    hook_addr = (uint32_t)(uintptr_t)ctx->hook_fn;

    /* ---------------------------------------------------------------
     * Step 1: Save original 4 instructions from target into trampoline.
     * --------------------------------------------------------------- */
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++) {
        ctx->tramp->instructions[i] = target[i];
    }

    /* ---------------------------------------------------------------
     * Step 2: Build branch-back at trampoline[4].
     *         After our 6-instruction patch (4 preamble + 2 extension),
     *         execution resumes at target + 24.
     * --------------------------------------------------------------- */
    back_addr = (uint32_t)(uintptr_t)ctx->target_addr + HOOK_NUM_INSTRUCTIONS * 4 + 8;
    ctx->tramp->branch_back = hook_build_ba((void *)(uintptr_t)back_addr);

    /* ---------------------------------------------------------------
     * Step 3: Write 4 NOPs at target as a safe window.
     *         This prevents partially-patched code from executing.
     * --------------------------------------------------------------- */
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++) {
        target[i] = hook_build_nop();
    }
    hook_flush_icache(ctx->target_addr, HOOK_NUM_INSTRUCTIONS * 4);

    /* ---------------------------------------------------------------
     * Step 4: Install preamble at target[0..3].
     *
     *   [0] mflr r0       — save LR
     *   [1] stw r2, 8(r1) — save game TOC
     *   [2] lis r11, hi   — load hook addr high
     *   [3] ori r11, lo   — load hook addr low
     * --------------------------------------------------------------- */
    target[0] = hook_build_mflr_r0();
    target[1] = hook_build_stw_r2();
    hook_build_load_r11(hook_addr, &target[2]);

    /* ---------------------------------------------------------------
     * Step 4b: Write mtctr/bctr at target+16 (patch extension).
     *
     *   [4] mtctr r11     — move hook addr to count register
     *   [5] bctr          — branch to count register
     * --------------------------------------------------------------- */
    {
        uint32_t *patch_ext = (uint32_t *)((uint8_t *)target +
                                            (HOOK_NUM_INSTRUCTIONS * 4));
        patch_ext[0] = hook_build_mtctr_r11();
        patch_ext[1] = hook_build_bctr();
    }

    /* ---------------------------------------------------------------
     * Step 5: Flush icache (6 instructions = 24 bytes patched).
     * --------------------------------------------------------------- */
    hook_flush_icache(ctx->target_addr, (HOOK_NUM_INSTRUCTIONS + 2) * 4);
    hook_flush_icache(ctx->tramp, HOOK_TRAMPOLINE_SIZE);

    DEBUG_PRINT("[HOOK] Installed: target=0x%08X -> hook=0x%08X\n",
                (unsigned)(uintptr_t)ctx->target_addr, hook_addr);
    DEBUG_PRINT("[HOOK] Trampoline at 0x%08X, branch-back to 0x%08X\n",
                (unsigned)(uintptr_t)ctx->tramp,
                (unsigned)back_addr);

    return 0;
}

/* ---------------------------------------------------------------
 * Hook removal
 * --------------------------------------------------------------- */

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

    /* Also NOP the extension (mtctr/bctr at target+16). */
    {
        uint32_t *patch_ext = (uint32_t *)((uint8_t *)target +
                                            (HOOK_NUM_INSTRUCTIONS * 4));
        patch_ext[0] = hook_build_nop();
        patch_ext[1] = hook_build_nop();
    }
    hook_flush_icache(ctx->target_addr, (HOOK_NUM_INSTRUCTIONS + 2) * 4);

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
