/**
 * toc_trampoline.s — DELETED 2026-07-22
 *
 * REPLACED BY: trampoline_gen.c (create_hook_trampoline)
 *
 * WHY: The PS3's 16-byte PLT stub does NOT dereference OPDs.
 * Writing an OPD address into the game's GOT triggers an ISI crash.
 * Dynamic trampolines generated at runtime solve this properly.
 *
 * All 4 hook wrappers and 3 passthrough stubs have been migrated
 * to trampoline_gen.c which generates 64-byte PowerPC trampolines
 * in an R-W-X page at runtime.
 *
 * Passthrough no longer needs assembly at all — hooks call real
 * cellUsbd* functions directly via the SPRX's own resolved imports.
 *
 * See: trampoline_gen.c, usb_hooks.c
 */

/* INTENTIONALLY EMPTY — kept as placeholder to prevent build confusion.
   Remove this file entirely from the Makefile's S_SRCS list. */
