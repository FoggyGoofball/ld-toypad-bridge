/**
 * usb_hooks.h — Fake cellUsbd Wrappers for Toy Pad Emulation
 *
 * Provides replacement functions for the LEGO Dimensions game's
 * cellUsbd imports. When the game calls cellUsbdInit, cellUsbdOpenPipe,
 * cellUsbdTransfer, or cellUsbdClosePipe, our hooks intercept the call
 * and route Toy Pad traffic to the network layer (Node.js server).
 *
 * Non-Toy-Pad traffic (controllers, storage, etc.) is forwarded to
 * the real LV2 kernel via trampolines.
 *
 * This file must be paired with hook.c for the detour installation.
 */

#ifndef USB_HOOKS_H
#define USB_HOOKS_H

#include <stdint.h>
#include "hook.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------- */

/** Maximum number of virtual pipe handles we can track. */
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

/**
 * Represents a virtual USB pipe that we've allocated.
 */
typedef struct {
    int      in_use;             /**< 1 if this slot is allocated */
    uint32_t pipe_handle;        /**< Fake pipe handle value */
    uint32_t dev_id;             /**< Device ID */
    uint8_t  ep_addr;            /**< Endpoint address (0x81 or 0x01) */
} usb_hook_pipe_t;

/**
 * Global state for the USB hook system.
 */
typedef struct {
    int               initialized;      /**< 1 after usb_hook_init() */
    hook_trampoline_t tramp_init;       /**< Trampoline for cellUsbdInit */
    hook_trampoline_t tramp_open_pipe;  /**< Trampoline for cellUsbdOpenPipe */
    hook_trampoline_t tramp_transfer;   /**< Trampoline for cellUsbdTransfer */
    hook_trampoline_t tramp_close_pipe; /**< Trampoline for cellUsbdClosePipe */
    usb_hook_pipe_t   pipes[USB_HOOK_MAX_PIPES]; /**< Virtual pipe pool */
    uint32_t          next_pipe_id;     /**< Monotonic counter for pipe handles */
    int               toypad_claimed;   /**< 1 if Toy Pad was detected */
    void              *prx_toc;         /**< Our PRX's TOC for preamble */
    hook_trampoline_t *tramp_init_ptr;  /**< Pointer to tramp_init for preamble */
} usb_hook_state_t;

extern usb_hook_state_t g_usb_hooks;

/* ---------------------------------------------------------------
 * Toy Pad Pipe Tracking
 * --------------------------------------------------------------- */

/**
 * Check if a pipe handle belongs to a Toy Pad endpoint.
 * Returns 1 if Toy Pad IN (0x81), 2 if Toy Pad OUT (0x01), 0 otherwise.
 */
int usb_hook_is_toypad_pipe(uint32_t pipe_handle);

/**
 * Look up a pipe by handle.
 * Returns pointer to pipe slot, or NULL if not found.
 */
usb_hook_pipe_t *usb_hook_lookup_pipe(uint32_t pipe_handle);

/* ---------------------------------------------------------------
 * Hook Functions (called directly by the game via detour)
 * --------------------------------------------------------------- */

/**
 * Hook for cellUsbdInit().
 * The game calls this to initialize the USB subsystem.
 * We return CELL_OK immediately to skip real USB init.
 *
 * Original signature:
 *   int cellUsbdInit(void);
 */
int my_cellUsbdInit(void);

/**
 * Hook for cellUsbdOpenPipe(pipe_handle, dev_id, ep_descriptor).
 * The game calls this to open a USB endpoint for communication.
 * We allocate a virtual pipe and return success.
 *
 * Original signature:
 *   int cellUsbdOpenPipe(uint32_t *pipe_handle, uint32_t dev_id,
 *                        void *ep_descriptor);
 */
int my_cellUsbdOpenPipe(uint32_t *pipe_handle, uint32_t dev_id,
                         void *ep_descriptor);

/**
 * Hook for cellUsbdTransfer(pipe_handle, ...).
 * THE KEY HOOK. Routes Toy Pad IN/OUT traffic to UDP network.
 *
 * For Toy Pad IN endpoint (0x81):
 *   - Sends poll request to PC server via network_send_poll()
 *   - Waits for response via network_recv()
 *   - Populates the game's buffer with spoofed tag data
 *
 * For Toy Pad OUT endpoint (0x01):
 *   - Forwards data to PC server via network_send_data()
 *
 * For non-Toy-Pad pipes:
 *   - Calls through trampoline to real LV2 USB driver
 *
 * Original signature:
 *   int cellUsbdTransfer(uint32_t pipe_handle, void *buf, uint32_t *len,
 *                        uint32_t arg4, uint32_t arg5);
 */
int my_cellUsbdTransfer(uint32_t pipe_handle, void *buf,
                         uint32_t *len, uint32_t arg4, uint32_t arg5);

/**
 * Hook for cellUsbdClosePipe(pipe_handle).
 * Frees our virtual pipe slot.
 *
 * Original signature:
 *   int cellUsbdClosePipe(uint32_t pipe_handle);
 */
int my_cellUsbdClosePipe(uint32_t pipe_handle);

/* ---------------------------------------------------------------
 * Initialization
 * --------------------------------------------------------------- */

/**
 * Install all USB hooks into the game process.
 *
 * This function must be called after the SPRX is loaded into the
 * LEGO Dimensions game process. It:
 *   1. Locates the game's cellUsbd function pointers
 *   2. Installs detour hooks on each function
 *   3. Initializes the virtual pipe pool
 *
 * The caller must provide the PRX's TOC (__toc) for preamble generation.
 *
 * @param prx_toc  Pointer to our PRX's .section __toc (the TOC base).
 *                 Can be obtained with: extern uint32_t __toc;
 * @return         0 on success, negative on error.
 */
int usb_hook_init(void *prx_toc);

/**
 * Remove all USB hooks and restore original functions.
 */
void usb_hook_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_HOOKS_H */
