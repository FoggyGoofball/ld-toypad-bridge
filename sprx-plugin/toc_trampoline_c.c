/**
 * toc_trampoline_c.c — DELETED 2026-07-22
 *
 * REPLACED BY: trampoline_gen.c + direct calls in usb_hooks.c
 *
 * This file previously contained:
 *   - Manually-constructed OPDs for passthrough stubs
 *   - call_original_OpenPipe/Transfer/ClosePipe function pointers
 *   - get_wrapper_*_addr() helper functions
 *
 * WHY REMOVED:
 *   1. Dynamic trampolines eliminate the need for assembly wrappers,
 *      so get_wrapper_*_addr() is no longer needed.
 *   2. Passthrough calls now go directly to cellUsbd* functions
 *      (SPRX's own resolved imports), eliminating the need for
 *      call_original_* OPD function pointers.
 *   3. No more OPD construction needed in C — create_hook_trampoline()
 *      extracts OPD data from the C hook function pointers directly.
 *
 * See: trampoline_gen.c, usb_hooks.c
 */

/* INTENTIONALLY EMPTY — kept as placeholder to prevent build confusion.
   Remove this file entirely from the Makefile's C_SRCS list. */
