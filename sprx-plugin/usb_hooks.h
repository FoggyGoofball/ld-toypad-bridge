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
 *   for each of 5 hooks, writes IPC file. The Node.js orchestrator reads
 *   the IPC file and writes 4-instruction preambles (lis/ori/mtctr/bctr)
 *   into the game's .text segment targeting each trampoline address.
 *
 *   Hooks: OpenPipe, InterruptTransfer, ClosePipe, GetDeviceDescriptor, ControlTransfer
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
#define HOOK_COUNT  5

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
    uint32_t          tramp_open_pipe_offset;
    uint32_t          tramp_transfer_offset;
    uint32_t          tramp_close_pipe_offset;
    uint32_t          tramp_get_device_desc_offset;  /**< cellUsbdGetDeviceDescriptor */
    uint32_t          tramp_control_transfer_offset;  /**< cellUsbdControlTransfer */
    uint32_t          tramp_reg_0944_offset;          /**< cellUsbdRegisterLdd @0x0944 */
    uint32_t          tramp_reg_0BB4_offset;          /**< cellUsbdRegisterExtraLdd? @0x0BB4 */
    uint32_t          tramp_reg_0D00_offset;          /**< cellUsbdRegisterExtraLdd? @0x0D00 */

    /* Heartbeat counter — stored in trampoline page at offset 512
     * (after 8 × 64-byte trampolines = 512 bytes). */
    volatile uint32_t *heartbeat;

    /* Stolen LDD ops — captured by RegisterLdd hook (NOT the Harvester).
     * Points to game's CellUsbdLddOps {name, probe, attach, detach} in .data.
     * Set by my_cellUsbdRegisterLdd_hook when game re-registers USB via XMB. */
    uint32_t          ldd_ops_addr;

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

/**
 * Hook for cellUsbdGetDeviceDescriptor(dev_id, desc, game_toc).
 * game_toc in r5 (2 original args in r3,r4).
 *
 * TROJAN HORSE STRATEGY (2026-07-26):
 * When a physical USB device is plugged into the PS3, the LV2 kernel
 * wakes all sleeping LDDs and invokes their probe() callbacks. The
 * game's probe() calls cellUsbdGetDeviceDescriptor() to identify the
 * device. This hook lies about the VID/PID, returning the ToyPad
 * descriptor (0x0E6F:0x0241) so the game accepts the physical device
 * as a ToyPad and proceeds to attach.
 */
int my_cellUsbdGetDeviceDescriptor(uint32_t dev_id, void *desc,
                                    uint32_t game_toc);

/**
 * Hook for cellUsbdControlTransfer(dev_handle, setup_pkt, buf, len, done_cb, user_data, game_toc).
 * game_toc in r9 (6 original args in r3-r8).
 */
int my_cellUsbdControlTransfer(uint32_t dev_handle, void *setup_pkt,
                                void *buf, uint32_t *length,
                                void *done_cb, void *user_data,
                                uint32_t game_toc);

/* ---------------------------------------------------------------
 * NID Constants
 *
 * Standard PS3 SDK NIDs for cellUsbd functions. Used by the NID
 * scanner (get_game_plt_stub) to locate the game's import stub table
 * entries. Also used by analyze_eboot.py for offline analysis.
 * --------------------------------------------------------------- */
#define NID_CELL_USBD_GET_DEVICE_DESC  0x9C8426F7U
#define NID_CELL_USBD_CONTROL_TRANSFER 0x3219460DU

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
