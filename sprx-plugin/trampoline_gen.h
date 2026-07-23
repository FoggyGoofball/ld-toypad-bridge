/**
 * trampoline_gen.h — Dynamic PowerPC Trampoline Generator
 *
 * Declares create_hook_trampoline(), which writes a 64-byte
 * (16-instruction) executable PowerPC trampoline into an R-W-X
 * memory page at runtime.
 *
 * WHY THIS EXISTS:
 *   The PS3's 16-byte PLT stub used for lazy binding does NOT
 *   dereference OPDs (Official Procedure Descriptors). It loads a
 *   raw code pointer from a GOT slot and branches to it directly:
 *
 *       lis   r11, got_offset_hi
 *       lwz   r12, got_offset_lo(r11)   ; r12 = raw code pointer
 *       mtctr r12
 *       bctr
 *
 *   If we write an OPD address into that GOT slot (as you would for
 *   a normal PowerPC function pointer call), the CPU will try to
 *   execute the OPD's data bytes (code_addr, toc_addr, env_ptr) as
 *   instructions — causing an immediate Instruction Storage
 *   Interrupt (ISI) crash.
 *
 *   The fix is to generate a small piece of REAL executable code
 *   (a trampoline) that:
 *     1. Saves the caller's TOC (r2) and link register
 *     2. Passes the caller's TOC as an extra argument to our C hook
 *        (so the hook can use it later for passthrough calls, if
 *        it constructs a temporary OPD)
 *     3. Loads OUR module's TOC into r2 (required so our C hook
 *        can correctly access its own GOT-relative globals)
 *     4. Branches-and-links to our hook's real code address
 *     5. Restores the caller's TOC and link register
 *     6. Returns to the caller
 *
 *   The trampoline's address (not an OPD!) is what gets written into
 *   the game's GOT slot. Since the trampoline is plain executable
 *   code (not a struct), bctr-ing into it is completely safe.
 *
 * (c) 2026 LD-ToyPad Bridge Team
 */

#ifndef TRAMPOLINE_GEN_H
#define TRAMPOLINE_GEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Size of a generated trampoline, in bytes (16 PowerPC instructions).
 */
#define TRAMPOLINE_GEN_SIZE  64

/**
 * Stack frame offset where the trampoline saves the caller's TOC
 * (r2) before loading our module's TOC. Useful if calling code
 * needs to know where to find the original TOC value on the stack.
 */
#define TRAMPOLINE_GEN_TOC_SAVE_OFFSET  0x28

/**
 * Generate a 64-byte PowerPC trampoline at the given executable
 * memory address.
 *
 * @param tramp         Destination buffer (must be R-W-X memory,
 *                       at least TRAMPOLINE_GEN_SIZE bytes, ideally
 *                       aligned to a cache line boundary).
 * @param c_func         Pointer to the C hook function to call. This
 *                       is a PowerPC function pointer, which on
 *                       CellOS actually points to a 12-byte OPD
 *                       struct { code_addr; toc_addr; env_ptr }.
 *                       create_hook_trampoline() dereferences this
 *                       OPD internally to obtain the real code
 *                       address and TOC value for our own SPRX
 *                       module — it does NOT execute the OPD as
 *                       code.
 * @param toc_arg_reg    PowerPC GPR number (3-10) in which to pass
 *                       the ORIGINAL caller's TOC value as an extra
 *                       trailing argument to c_func. Choose this
 *                       based on how many real arguments c_func
 *                       already has:
 *                         0 args -> r3
 *                         1 arg  -> r4
 *                         2 args -> r5
 *                         3 args -> r6
 *                         4 args -> r7
 *                         5 args -> r8
 *
 * After calling this function, the memory at `tramp` contains fully
 * executable code — both the data and instruction caches have been
 * flushed/invalidated so the PPU will fetch the correct bytes.
 */
void create_hook_trampoline(uint32_t *tramp, void *c_func, int toc_arg_reg);

#ifdef __cplusplus
}
#endif

#endif /* TRAMPOLINE_GEN_H */
