/**
 * usb_hooks.h — REFACTORED 2026-07-22 (Dynamic Trampoline Generation)
 *
 * MAJOR CHANGES:
 *   1. Removed all assembly wrapper/passthrough references
 *   2. Removed target_*_addr fields (OPD trick extracts raw code ptrs
 *      but they're only used locally in usb_hooks.c)
 *   3. Added trampoline_base field (single R-W-X allocation)
 *   4. Added trampoline_*_offset fields (64-byte aligned offsets into page)
 *   5. Removed call_original_* externs — hooks call real cellUsbd directly
 *   6. Removed get_wrapper_*_addr helpers — trampoline addresses are
 *      passed directly to Node.js via IPC file
 *
 * ARCHITECTURE:
 *   usb_hook_init() allocates 1 R-W-X page, calls create_hook_trampoline()
 *   for each of 4 hooks, writes IPC file. The Node.js orchestrator reads
 *   the IPC file and writes 4-instruction preambles (lis/ori/mtctr/bctr)
 *   into the game's .text segment targeting each trampoline address.
 *
 *   Passthrough: Non-ToyPad USB calls just call the real cellUsbd*
 *   functions directly. The SPRX has its own resolved imports from
 *   -lusbd_stub, so calling cellUsbdOpenPipe() from C uses the SPRX's
 *   GOT/TOC and never touches the game's memory.
 */

#ifndef USB_HOOKS_H
#define USB_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------- */

#define USB_HOOK_MAX_PIPES  8

/** Toy Pad endpoint addresses. */
#define TOYPAD_EP_IN       0x81   /**< Interrupt IN  (device -> PS3) */
#define TOYPAD_EP_OUT      0x01   /**< Interrupt OUT (PS3 -> device) */

/** Toy Pad USB identifiers. */
#define TOYPAD_VID         0x0E6F
#define TOYPAD_PID         0x0241

/** Size of each trampoline in bytes (16 instructions) */
#define HOOK_TRAMPOLINE_SIZE  64

/** Stack frame offset where trampoline saves game's TOC (r2) */
#define HOOK_TOC_SAVE_OFFSET   0x28

/** Number of hooks to install */
#define HOOK_COUNT  4

/* ---------------------------------------------------------------
 * Types
 * --------------------------------------------------------------- */

typedef struct {
    int      in_use;             /**< 1 if this slot is allocated */
    uint32_t pipe_handle;        /**< Fake pipe handle value */
    uint32_t dev_id;             /**< Device ID */
    uint8_t  ep_addr;            /**< Endpoint address (0x81 or 0x01) */
} usb_hook_pipe_t;

/**
 * Global state for the USB hook system.
 * REFACTORED 2026-07-22: Dynamic trampoline generation.
 */
typedef struct {
    int               initialized;          /**< 1 after usb_hook_init() */

    /* Trampoline page base address (from sys_memory_allocate R-W-X) */
    uint32_t          trampoline_base;

    /* Trampoline offsets within the page (64-byte aligned) */
    uint32_t          tramp_init_offset;
    uint32_t          tramp_open_pipe_offset;
    uint32_t          tramp_transfer_offset;
    uint32_t          tramp_close_pipe_offset;

    /* Heartbeat counter — stored in trampoline page at offset 256
     * (after 4 × 64-byte trampolines = 256 bytes). Incremented each
     * worker loop iteration (~20 Hz at 50ms sleep). Polled by Node.js
     * orchestrator via PS3MAPI /read_process. */
    volatile uint32_t *heartbeat;

    usb_hook_pipe_t   pipes[USB_HOOK_MAX_PIPES]; /**< Virtual pipe pool */
    uint32_t          next_pipe_id;     /**< Monotonic counter for pipe handles */
    int               toypad_claimed;   /**< 1 if Toy Pad was detected */
} usb_hook_state_t;

extern usb_hook_state_t g_usb_hooks;

/* ---------------------------------------------------------------
 * Toy Pad Pipe Tracking
 * --------------------------------------------------------------- */

int usb_hook_is_toypad_pipe(uint32_t pipe_handle);
usb_hook_pipe_t *usb_hook_lookup_pipe(uint32_t pipe_handle);

/* ---------------------------------------------------------------
 * Hook Functions
 *
 * All signatures include game_toc as LAST argument.
 * TOC is passed by the dynamic trampoline (trampoline_gen.c) — NOT
 * by assembly wrappers.
 * --------------------------------------------------------------- */

/**
 * Hook for cellUsbdInit().
 * game_toc in r3 (0 original args).
 */
int my_cellUsbdInit(uint32_t game_toc);

/**
 * Hook for cellUsbdOpenPipe(pipe_handle, dev_id, ep_descriptor, game_toc).
 * game_toc in r6 (3 original args in r3,r4,r5).
 */
int my_cellUsbdOpenPipe(uint32_t *pipe_handle, uint32_t dev_id,
                         void *ep_descriptor, uint32_t game_toc);

/**
 * Hook for cellUsbdInterruptTransfer(pipe_handle, buf, len, done_cb, arg, game_toc).
 * game_toc in r8 (5 original args in r3-r7).
 */
int my_cellUsbdInterruptTransfer(uint32_t pipe_handle, void *buf,
                                  uint32_t *len, void *done_cb, void *arg,
                                  uint32_t game_toc);

/**
 * Hook for cellUsbdClosePipe(pipe_handle, game_toc).
 * game_toc in r4 (1 original arg in r3).
 */
int my_cellUsbdClosePipe(uint32_t pipe_handle, uint32_t game_toc);

/* ---------------------------------------------------------------
 * Initialization / Shutdown
 *
 * usb_hook_init():
 *   1. Extracts cellUsbd code addresses from SPRX OPDs
 *   2. Allocates 1 R-W-X page via sys_memory_allocate
 *   3. Calls create_hook_trampoline() for each of 4 hooks
 *   4. Writes IPC file for Node.js orchestrator
 *   5. Returns 0 on success, -1 on failure
 *
 * usb_hook_shutdown():
 *   Writes shutdown IPC file, resets state
 * --------------------------------------------------------------- */

int usb_hook_init(void);
void usb_hook_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_HOOKS_H */
