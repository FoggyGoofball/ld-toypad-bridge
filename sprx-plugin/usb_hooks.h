/**
 * usb_hooks.h — REFACTORED 2026-07-20
 *
 * Provides replacement functions for the LEGO Dimensions game's
 * cellUsbd imports. When the game calls cellUsbdInit, cellUsbdOpenPipe,
 * cellUsbdTransfer, or cellUsbdClosePipe, our hooks intercept the call
 * and route Toy Pad traffic to the network layer (Node.js server).
 *
 * REFACTOR CHANGES:
 *   - No more hook_trampoline_t types (removed hook.h dependency)
 *   - uint32_t tramp_*_addr replaces hook_trampoline_t
 *   - Function signatures updated: game_toc is LAST argument
 *   - No more prx_toc param on usb_hook_init()
 *   - Added target_*_addr fields for NID scan results
 *   - No more hook_install/remove — preamble written by PS3MAPI from Ring 0
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
 * REFACTORED: hook_trampoline_t replaced with raw uint32_t addresses.
 */
typedef struct {
    int               initialized;          /**< 1 after usb_hook_init() */

    /* NID scan results: target function addresses in game .text */
    uint32_t          target_init_addr;
    uint32_t          target_open_pipe_addr;
    uint32_t          target_transfer_addr;
    uint32_t          target_close_pipe_addr;

    /* Trampoline addresses (allocated via sys_memory_allocate R-W-X) */
    uint32_t          tramp_init_addr;
    uint32_t          tramp_open_pipe_addr;
    uint32_t          tramp_transfer_addr;
    uint32_t          tramp_close_pipe_addr;

    /* Heartbeat counter — stored in the sys_memory_allocate page at offset 128
     * (after 4 × 32-byte trampoline blocks). Incremented each worker loop
     * iteration (~20 Hz at 50ms sleep). Polled by Node.js orchestrator via
     * PS3MAPI /read_process. No HDD writes — avoids I/O contention with game
     * asset streaming that would trigger the trophy freeze. */
    volatile uint32_t *heartbeat;       /**< Memory-mapped heartbeat pointer */

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
 * REFACTORED: All signatures updated to include game_toc as LAST arg.
 * TOC is passed by the assembly wrapper (toc_trampoline.s) — NOT inline asm.
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
 * Initialization
 *
 * REFACTORED: No more prx_toc param.
 * usb_hook_init() performs NID scan, allocates trampoline pages,
 * copies original instructions, and writes IPC file for Node.js.
 * The actual preamble is written by PS3MAPI from Ring 0.
 * --------------------------------------------------------------- */

int usb_hook_init(void);
void usb_hook_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_HOOKS_H */
