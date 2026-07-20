/**
 * usb_hooks.c — Fake cellUsbd Wrappers for Toy Pad Emulation
 *
 * FIXES APPLIED (per HANDOFF-2026-07-20-CRITICAL-BUGS):
 *   1. Defect 3 (GOT Resolution): scan_for_nid() now properly reads the
 *      got_ptr field from 12-byte import table entries and dereferences it
 *      to extract the true function pointer.
 *   2. Defect 4 (Passthrough TOC Collisions): Non-Toy-Pad passthrough calls
 *      now use inline PowerPC assembly (TOC_CALL_TRAMPOLINE macro) to
 *      restore the game's r2 before calling through the trampoline.
 *   3. Defect 1 (Instruction Overflow): removed prx_toc from hook_ctx_t
 *      references — preamble now correctly saves r2 to stack.
 *
 * Architecture:
 *   my_cellUsbdInit()        -> return CELL_OK (skip real USB init)
 *   my_cellUsbdOpenPipe()    -> allocate virtual pipe handle
 *   my_cellUsbdTransfer()    -> IN: poll server / OUT: forward to server
 *   my_cellUsbdClosePipe()   -> free virtual pipe handle
 *
 * NID-based function discovery (FIXED Defect 3):
 *   Scans game memory for 12-byte import table entries where the first
 *   uint32_t matches the target NID. The third uint32_t (got_ptr) is the
 *   address of the GOT slot containing the resolved function pointer.
 *   We dereference got_ptr to obtain the true function address.
 *
 * TOC management (FIXED Defect 4):
 *   The preamble (hook.c) saves the game's r2 (TOC) to the game's stack
 *   frame at offset 8 before branching to our C function. The C function
 *   prologue (-mprx) automatically switches r2 to our PRX's TOC. For
 *   passthrough calls (non-Toy-Pad), we use inline asm to:
 *     1. Save our PRX's r2
 *     2. Restore the game's r2 from the preamble's save location
 *     3. Call the trampoline
 *     4. Restore our PRX's r2
 */

#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "hook.h"
#include "usb_hooks.h"
#include "network.h"
#include "debug.h"
#include "syscall.h"

/* ---------------------------------------------------------------
 * Global state
 * --------------------------------------------------------------- */
usb_hook_state_t g_usb_hooks;

/* ---------------------------------------------------------------
 * Token USB error/status codes
 * --------------------------------------------------------------- */
#define CELL_OK                    0
#define CELL_USBD_ERROR_FAILED    -1

/* ---------------------------------------------------------------
 * Known cellUsbd NID Values (FNV-1a 32-bit, masked to 0x7FFFFFFF)
 *
 * These are the Name Identifiers that the game's PRX import table
 * uses to reference cellUsbd functions. Each entry in the import
 * table is a 12-byte structure:
 *   struct { uint32_t nid; uint32_t reserved; uint32_t got_ptr; }
 *
 * The got_ptr field points to a GOT slot that, after module loading,
 * contains the runtime address of the function.
 * --------------------------------------------------------------- */
#define NID_CELL_USBD_INIT          0x7F5F00D3U
#define NID_CELL_USBD_OPEN_PIPE     0x1AB6D80BU
#define NID_CELL_USBD_TRANSFER      0x7B4436CEU
#define NID_CELL_USBD_CLOSE_PIPE    0x2F82F1A5U

/* ---------------------------------------------------------------
 * PRX Import Table Entry Structure
 *
 * Each loaded PRX module has an import table. Entries are 12 bytes:
 *   offset +0: uint32_t nid       (FNV-1a hash)
 *   offset +4: uint32_t reserved  (flags/alignment, usually 0)
 *   offset +8: uint32_t got_ptr   (pointer to GOT slot with func addr)
 *
 * We search the game's main memory regions for import table entries
 * matching our NIDs, then read got_ptr to find the GOT slot, then
 * dereference the GOT slot to get the real function address.
 * --------------------------------------------------------------- */

/** Number of bytes to search per chunk (not used with direct scan). */
#define NID_SCAN_CHUNK_SIZE  0x10000  /* 64KB */

/** Candidate memory regions to scan for import tables.
 *  These are typical locations where PRX import stubs and GOT entries
 *  reside in LEGO Dimensions (BLUS31548 / BLES02206).
 *  The SPRX has full memory access once injected via PS3MAPI,
 *  so we search likely .text / .got2 / .data ranges. */
static const uint32_t g_scan_regions[][2] = {
    { 0x00100000, 0x00800000 },   /* Game .text + .got2 (main EBOOT region) */
    { 0x01000000, 0x01000000 },   /* Extended code segment */
    { 0x02000000, 0x01000000 },   /* High code / PRX load area */
    { 0x30000000, 0x00800000 },   /* Alternative PRX region */
    { 0x40000000, 0x01000000 },   /* Large PRX / plugin area */
};
#define NUM_SCAN_REGIONS  (sizeof(g_scan_regions) / sizeof(g_scan_regions[0]))

/* ---------------------------------------------------------------
 * TOC Management Helpers
 *
 * The preamble (hook.c) saves the game's r2 (TOC) to the game's stack
 * frame at offset 8 before branching to our C function. The game's stack
 * frame is accessible through the backchain: the word at *(uint32_t*)r1
 * is the pointer to the caller's (game's) stack frame.
 *
 * The preamble instructions at target[0..3]:
 *   [0] mflr r0           ; save LR
 *   [1] stw r2, 8(r1)     ; save game's TOC at (game's r1 + 8)
 *   [2] lis r11, <hi>     ; load hook address high
 *   [3] ori r11, <lo>     ; load hook address low
 *   [4] mtctr r11         ; move to count register
 *   [5] bctr              ; branch to hook
 *
 * When our C function runs (after -mprx prologue), r1 points to our
 * stack frame. The game's old r1 is at *(uint32_t*)r1 (backchain).
 * The game's TOC is at *(uint32_t*)(old_r1 + 8).
 * --------------------------------------------------------------- */

/** Offset where preamble saves the game's r2 (TOC) in the caller's frame. */
#define HOOK_TOC_SAVE_OFFSET  8

/**
 * Retrieve the game's TOC value that was saved by our 4-instruction preamble.
 */
static inline uint32_t get_game_toc(void)
{
    uint32_t caller_r1;
    uint32_t game_toc;

    /* Read backchain to get caller's (game's) stack pointer. */
    __asm__ volatile (
        "lwz %0, 0(%%r1)\n\t"
        : "=r"(caller_r1)
    );

    /* Read game's TOC saved by preamble at (old SP + 8). */
    __asm__ volatile (
        "lwz %0, %1(%2)\n\t"
        : "=r"(game_toc)
        : "i"(HOOK_TOC_SAVE_OFFSET), "r"(caller_r1)
    );

    return game_toc;
}

/**
 * Macro: Call a function via a trampoline pointer, properly switching
 * TOC (r2) between PRX and game contexts.
 *
 * Steps:
 *   1. Save our PRX's r2 (TOC) to a local variable
 *   2. Restore the game's r2 from the preamble save location
 *   3. Call the trampoline function (which executes original game code)
 *   4. Restore our PRX's r2 before using any global variables
 *
 * @param ret_var   Integer variable to store the return value.
 * @param fn_ptr    Function pointer (trampoline address, cast to proper type).
 * @param fn_type   Function pointer type (e.g. tramp_fn_t).
 * @param args...   Arguments to pass to the function.
 */
#define TOC_CALL_TRAMPOLINE(ret_var, fn_ptr, fn_type, args...) \
    do { \
        uint32_t saved_our_r2; \
        uint32_t game_r2; \
        /* Step 1: Save our PRX's TOC */ \
        __asm__ volatile ( \
            "mr %0, %%r2\n\t" \
            : "=r"(saved_our_r2) \
        ); \
        /* Step 2: Get the game's TOC */ \
        game_r2 = get_game_toc(); \
        /* Step 3: Switch to game's TOC and call trampoline */ \
        __asm__ volatile ( \
            "mr %%r2, %0\n\t" \
            : : "r"(game_r2) \
        ); \
        ret_var = ((fn_type)(uintptr_t)(fn_ptr))(args); \
        /* Step 4: Restore our PRX's TOC */ \
        __asm__ volatile ( \
            "mr %%r2, %0\n\t" \
            : : "r"(saved_our_r2) \
        ); \
    } while (0)

/* ---------------------------------------------------------------
 * Toy Pad Pipe Tracking
 * --------------------------------------------------------------- */

int usb_hook_is_toypad_pipe(uint32_t pipe_handle)
{
    int i;
    for (i = 0; i < USB_HOOK_MAX_PIPES; i++) {
        if (g_usb_hooks.pipes[i].in_use &&
            g_usb_hooks.pipes[i].pipe_handle == pipe_handle) {
            /* Return 1 for IN, 2 for OUT */
            if (g_usb_hooks.pipes[i].ep_addr == TOYPAD_EP_IN)
                return 1;
            if (g_usb_hooks.pipes[i].ep_addr == TOYPAD_EP_OUT)
                return 2;
            return 0;
        }
    }
    return 0;
}

usb_hook_pipe_t *usb_hook_lookup_pipe(uint32_t pipe_handle)
{
    int i;
    for (i = 0; i < USB_HOOK_MAX_PIPES; i++) {
        if (g_usb_hooks.pipes[i].in_use &&
            g_usb_hooks.pipes[i].pipe_handle == pipe_handle) {
            return &g_usb_hooks.pipes[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------
 * Internal: Allocate a virtual pipe slot
 * --------------------------------------------------------------- */
static usb_hook_pipe_t *alloc_pipe(void)
{
    int i;
    uint32_t handle;

    handle = ++g_usb_hooks.next_pipe_id;
    if (handle == 0) handle = ++g_usb_hooks.next_pipe_id; /* skip 0 */

    for (i = 0; i < USB_HOOK_MAX_PIPES; i++) {
        if (!g_usb_hooks.pipes[i].in_use) {
            g_usb_hooks.pipes[i].in_use = 1;
            g_usb_hooks.pipes[i].pipe_handle = handle;
            g_usb_hooks.pipes[i].dev_id = 0;
            g_usb_hooks.pipes[i].ep_addr = 0;
            return &g_usb_hooks.pipes[i];
        }
    }

    DEBUG_ERROR("[USB] No free pipe slots!\n");
    return NULL;
}

/* ---------------------------------------------------------------
 * Internal: Free a virtual pipe slot
 * --------------------------------------------------------------- */
static void free_pipe(uint32_t pipe_handle)
{
    usb_hook_pipe_t *pipe = usb_hook_lookup_pipe(pipe_handle);
    if (pipe) {
        DEBUG_PRINT("[USB] Freed pipe handle 0x%08X (ep=0x%02X)\n",
                    (unsigned)pipe_handle, (unsigned)pipe->ep_addr);
        memset(pipe, 0, sizeof(*pipe));
    }
}

/* ---------------------------------------------------------------
 * Internal: Extract endpoint address from a standard USB endpoint
 * descriptor. The descriptor is 7 bytes:
 *   byte 0: bLength (0x07)
 *   byte 1: bDescriptorType (0x05 = ENDPOINT)
 *   byte 2: bEndpointAddress  (bit 7 = direction, bits 3-0 = number)
 *   byte 3: bmAttributes
 *   byte 4-5: wMaxPacketSize
 *   byte 6: bInterval
 * --------------------------------------------------------------- */
static uint8_t extract_ep_addr(const void *ep_descriptor)
{
    if (ep_descriptor == NULL) {
        return 0;
    }
    /* Endpoint address is at byte offset 2 in the descriptor. */
    return ((const uint8_t *)ep_descriptor)[2];
}

/* ---------------------------------------------------------------
 * Hook: my_cellUsbdInit
 *
 * The game calls this to initialize the USB subsystem.
 * We return CELL_OK immediately to skip real USB init.
 * --------------------------------------------------------------- */
int my_cellUsbdInit(void)
{
    DEBUG_PRINT("[USB] cellUsbdInit() intercepted returning CELL_OK\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------
 * Hook: my_cellUsbdOpenPipe
 *
 * For Toy Pad endpoints (0x81 IN, 0x01 OUT), allocate a virtual pipe.
 * For non-Toy-Pad endpoints, pass through to the real LV2 USB driver
 * via the trampoline with proper TOC switching.
 *
 * Original signature:
 *   int cellUsbdOpenPipe(uint32_t *pipe_handle, uint32_t dev_id,
 *                        void *ep_descriptor);
 * --------------------------------------------------------------- */
int my_cellUsbdOpenPipe(uint32_t *pipe_handle, uint32_t dev_id,
                         void *ep_descriptor)
{
    uint8_t ep_addr;
    usb_hook_pipe_t *pipe;

    if (pipe_handle == NULL) {
        DEBUG_ERROR("[USB] cellUsbdOpenPipe: NULL pipe_handle\n");
        return CELL_USBD_ERROR_FAILED;
    }

    ep_addr = extract_ep_addr(ep_descriptor);

    DEBUG_PRINT("[USB] cellUsbdOpenPipe intercepted: dev_id=%u, ep=0x%02X\n",
                (unsigned)dev_id, (unsigned)ep_addr);

    /* For Toy Pad endpoints, allocate a virtual pipe. */
    if (ep_addr == TOYPAD_EP_IN || ep_addr == TOYPAD_EP_OUT) {
        pipe = alloc_pipe();
        if (pipe == NULL) {
            DEBUG_ERROR("[USB] Failed to allocate pipe for Toy Pad ep=0x%02X\n",
                        (unsigned)ep_addr);
            return CELL_USBD_ERROR_FAILED;
        }

        pipe->dev_id = dev_id;
        pipe->ep_addr = ep_addr;
        *pipe_handle = pipe->pipe_handle;

        g_usb_hooks.toypad_claimed = 1;

        DEBUG_PRINT("[USB] Toy Pad pipe opened: handle=0x%08X, ep=0x%02X\n",
                    (unsigned)pipe->pipe_handle, (unsigned)ep_addr);
        return CELL_OK;
    }

    /*
     * FIXED (Defect 4): Non-Toy-Pad passthrough now uses TOC switching.
     * The game's TOC (r2) was saved by our preamble at (game's SP + 8).
     * We call the trampoline with the game's TOC in r2, then restore
     * our PRX's TOC afterward.
     */
    {
        typedef int (*tramp_fn_t)(uint32_t*, uint32_t, void*);
        int ret;

        DEBUG_VERBOSE("[USB] Non-Toy-Pad pipe open: passthrough with TOC switch\n");

        TOC_CALL_TRAMPOLINE(ret, &g_usb_hooks.tramp_open_pipe, tramp_fn_t,
                            pipe_handle, dev_id, ep_descriptor);

        return ret;
    }
}

/* ---------------------------------------------------------------
 * Hook: my_cellUsbdTransfer
 *
 * THE KEY HOOK. Handles USB data transfers.
 *
 * For Toy Pad IN (0x81): Poll server for tag data.
 * For Toy Pad OUT (0x01): Forward data to server.
 * For non-Toy-Pad: Passthrough to real LV2 via trampoline with TOC switch.
 *
 * Original signature (typical):
 *   int cellUsbdTransfer(uint32_t pipe_handle, void *buf, uint32_t *len,
 *                        uint32_t arg4, uint32_t arg5);
 * --------------------------------------------------------------- */
int my_cellUsbdTransfer(uint32_t pipe_handle, void *buf,
                         uint32_t *len, uint32_t arg4, uint32_t arg5)
{
    int toypad_type;

    (void)arg4;
    (void)arg5;

    /* Check if this is a Toy Pad pipe. */
    toypad_type = usb_hook_is_toypad_pipe(pipe_handle);

    if (toypad_type == 0) {
        /*
         * FIXED (Defect 4): Non-Toy-Pad passthrough with TOC switching.
         */
        typedef int (*tramp_fn_t)(uint32_t, void*, uint32_t*, uint32_t, uint32_t);
        int ret;

        DEBUG_VERY_VERBOSE("[USB] Non-Toy-Pad transfer: passthrough\n");

        TOC_CALL_TRAMPOLINE(ret, &g_usb_hooks.tramp_transfer, tramp_fn_t,
                            pipe_handle, buf, len, arg4, arg5);

        return ret;
    }

    if (toypad_type == 1) {
        /* Toy Pad IN endpoint (0x81): Poll server for tag data. */
        uint32_t max_len;
        uint8_t response[NET_PACKET_MAX_SIZE];
        uint8_t zone = 1;  /* Default zone: CENTER */
        uint8_t seq;
        int recv_len;

        if (buf == NULL || len == NULL) {
            DEBUG_ERROR("[USB] IN transfer with NULL buffer/len\n");
            return CELL_USBD_ERROR_FAILED;
        }

        max_len = *len;
        seq = (uint8_t)(g_usb_hooks.next_pipe_id++); /* cheap seq */

        /* Send poll request to PC server. */
        if (network_send_poll(zone, seq) < 0) {
            /* No server known yet - return empty response. */
            if (max_len > 0) {
                memset(buf, 0, max_len);
            }
            *len = 0;
            return CELL_OK;
        }

        /* Try to receive response (non-blocking with MSG_DONTWAIT). */
        recv_len = network_recv(response, sizeof(response));

        if (recv_len > 2 && response[0] == 0x00 /* RESPONSE_OK */) {
            /* Valid response from server - copy tag data. */
            int payload_len = recv_len - 3;
            if (payload_len > (int)max_len) {
                payload_len = (int)max_len;
            }
            if (payload_len > 0) {
                memcpy(buf, response + 3, (size_t)payload_len);
            }
            *len = (uint32_t)payload_len;
            DEBUG_VERBOSE("[USB] IN transfer: got %d bytes from server\n",
                         payload_len);
        } else if (recv_len > 2 && response[0] == 0x01 /* RESPONSE_NO_TAG */) {
            /* No tag on zone - return empty with report ID. */
            if (max_len > 0) {
                memset(buf, 0, max_len);
                ((uint8_t*)buf)[0] = 0x01;  /* HID report ID */
            }
            *len = max_len;
        } else {
            /* No response or error - return empty poll result. */
            if (max_len > 0) {
                memset(buf, 0, max_len);
                ((uint8_t*)buf)[0] = 0x01;  /* HID report ID = pad status */
            }
            *len = max_len;
            DEBUG_VERY_VERBOSE("[USB] IN transfer: no server response\n");
        }

        return CELL_OK;
    }

    if (toypad_type == 2) {
        /* Toy Pad OUT endpoint (0x01): Forward data to server. */
        uint32_t data_len;
        uint8_t zone = 1;  /* Default zone: CENTER */
        uint8_t seq;

        if (buf == NULL || len == NULL) {
            DEBUG_ERROR("[USB] OUT transfer with NULL buffer/len\n");
            return CELL_USBD_ERROR_FAILED;
        }

        data_len = *len;
        seq = (uint8_t)(g_usb_hooks.next_pipe_id++);

        DEBUG_PACKET("[USB] OUT transfer: pipe=0x%08X, len=%u\n",
                     (unsigned)pipe_handle, (unsigned)data_len);
        DEBUG_HEX_DUMP("[USB] OUT data", (const uint8_t*)buf, (int)data_len);

        /* Forward to PC server via UDP. */
        network_send_data(zone, seq, (const uint8_t*)buf, (int)data_len);

        return CELL_OK;
    }

    /* Should never reach here. */
    DEBUG_ERROR("[USB] Unhandled transfer: pipe=0x%08X, type=%d\n",
                (unsigned)pipe_handle, toypad_type);
    return CELL_USBD_ERROR_FAILED;
}

/* ---------------------------------------------------------------
 * Hook: my_cellUsbdClosePipe
 *
 * For Toy Pad pipes, frees our virtual pipe slot.
 * For non-Toy-Pad pipes, passes through with TOC switching.
 *
 * Original signature:
 *   int cellUsbdClosePipe(uint32_t pipe_handle);
 * --------------------------------------------------------------- */
int my_cellUsbdClosePipe(uint32_t pipe_handle)
{
    int toypad_type;

    toypad_type = usb_hook_is_toypad_pipe(pipe_handle);

    if (toypad_type == 0) {
        /*
         * FIXED (Defect 4): Non-Toy-Pad passthrough with TOC switching.
         */
        typedef int (*tramp_fn_t)(uint32_t);
        int ret;

        DEBUG_VERBOSE("[USB] Non-Toy-Pad close pipe: passthrough\n");

        TOC_CALL_TRAMPOLINE(ret, &g_usb_hooks.tramp_close_pipe, tramp_fn_t,
                            pipe_handle);

        return ret;
    }

    /* Toy Pad pipe: free our virtual slot. */
    DEBUG_PRINT("[USB] cellUsbdClosePipe: Toy Pad handle=0x%08X\n",
                (unsigned)pipe_handle);
    free_pipe(pipe_handle);
    return CELL_OK;
}

/* ---------------------------------------------------------------
 * FIXED: NID-Based Function Address Discovery (Defect 3)
 *
 * The PS3 PRX import table is an array of 12-byte entries:
 *   struct prx_import_entry {
 *       uint32_t nid;        // FNV-1a hash (big-endian in memory)
 *       uint32_t reserved;   // flags/alignment (usually 0)
 *       uint32_t got_ptr;    // POINTER to the GOT slot
 *   };
 *
 * The got_ptr field is a pointer to a GOT (Global Offset Table) entry
 * that, after module loading, contains the runtime address of the
 * imported function. So:
 *   - entry.got_ptr  = address of the GOT slot
 *   - *(entry.got_ptr) = actual function address
 *
 * FIX: Previously, the scanner would find the NID but return NULL for
 * the function address because it tried to compute (nid_high << 16) + offset
 * which is nonsensical. Now it properly reads got_ptr from the import
 * entry and dereferences it to get the true function pointer.
 * --------------------------------------------------------------- */

/** Structure to hold a matched NID and its resolved address. */
typedef struct {
    uint32_t nid;            /**< The NID we were searching for */
    void     *func_addr;     /**< Resolved function address (from GOT) */
    int      found;          /**< 1 if successfully found */
} nid_match_t;

/**
 * Scan a memory range for a specific NID in the PRX import table.
 *
 * We search for the pattern of a 12-byte import entry:
 *   [0] uint32_t nid       (must == target_nid)
 *   [1] uint32_t reserved  (should be 0 or small value)
 *   [2] uint32_t got_ptr   (must be a valid memory address)
 *
 * When found, we dereference got_ptr to extract the function address.
 *
 * @param start      Start address of range to scan.
 * @param size       Size of range in bytes.
 * @param target_nid The NID to search for.
 * @param out_addr   Output: the resolved function address.
 * @return 0 on success, -1 if not found.
 */
static int scan_for_nid(void *start, uint32_t size, uint32_t target_nid,
                         void **out_addr)
{
    uint32_t *words = (uint32_t*)start;
    uint32_t nwords = size / sizeof(uint32_t);
    uint32_t i;

    /*
     * The import table is an array of 12-byte (3-word) entries.
     * We scan 3 words at a time:
     *   words[i+0] = nid
     *   words[i+1] = reserved (should be 0)
     *   words[i+2] = got_ptr  (pointer to GOT slot)
     *
     * FIXED (Defect 3): We no longer attempt to compute the address from
     * the lis/ori NID value. Instead, we read the got_ptr field directly
     * from the import table entry and dereference it.
     */
    for (i = 0; i <= nwords - 3; i += 3) {
        uint32_t nid       = words[i + 0];
        uint32_t reserved  = words[i + 1];
        uint32_t got_ptr   = words[i + 2];

        /* Skip mismatched NIDs. */
        if (nid != target_nid)
            continue;

        /*
         * FOUND a potential import entry. Validate it:
         *   - reserved should be 0 or a small flag value
         *   - got_ptr should point to valid readable memory
         *   - got_ptr should be in a reasonable address range
         */
        if ((reserved != 0 && reserved > 0x1000)) {
            /* reserved looks wrong - might be false positive */
            DEBUG_VERBOSE("[USB] NID 0x%08X at +0x%X: skipped (reserved=0x%X)\n",
                         (unsigned)target_nid,
                         (unsigned)(i * 4),
                         (unsigned)reserved);
            continue;
        }

        if (got_ptr == 0 || got_ptr < 0x00010000 || got_ptr > 0x4FFFFFFF) {
            /* got_ptr not in valid PS3 memory range */
            DEBUG_VERBOSE("[USB] NID 0x%08X at +0x%X: skipped (got_ptr=0x%X)\n",
                         (unsigned)target_nid,
                         (unsigned)(i * 4),
                         (unsigned)got_ptr);
            continue;
        }

        /*
         * Dereference the GOT slot to get the actual function address.
         * We're in the game's address space, so direct pointer access works.
         *
         * The GOT slot contains a pointer to the function's implementation
         * (e.g., a syscall stub or actual LV2 function entry).
         */
        {
            uint32_t *got_slot = (uint32_t*)(uintptr_t)got_ptr;
            uint32_t func_addr = *got_slot;

            if (func_addr == 0 || func_addr < 0x00010000) {
                /* GOT slot not yet resolved (module might not be loaded) */
                DEBUG_VERBOSE("[USB] NID 0x%08X: GOT slot 0x%08X not resolved\n",
                             (unsigned)target_nid,
                             (unsigned)got_ptr);
                continue;
            }

            *out_addr = (void*)(uintptr_t)func_addr;

            DEBUG_PRINT("[USB] NID 0x%08X: GOT=0x%08X => func=0x%08X\n",
                        (unsigned)target_nid,
                        (unsigned)got_ptr,
                        (unsigned)func_addr);

            return 0;
        }
    }

    /* NID not found in this range. */
    return -1;
}

/**
 * Locate cellUsbd function addresses by scanning for known NIDs
 * in the game's PRX import table.
 *
 * @param out_init        Output: address of cellUsbdInit.
 * @param out_open_pipe   Output: address of cellUsbdOpenPipe.
 * @param out_transfer    Output: address of cellUsbdTransfer.
 * @param out_close_pipe  Output: address of cellUsbdClosePipe.
 * @return 0 on success, -1 on failure.
 */
static int find_game_cellusbd_functions(void **out_init,
                                         void **out_open_pipe,
                                         void **out_transfer,
                                         void **out_close_pipe)
{
    nid_match_t targets[4];
    unsigned int r, attempt;
    int all_found;
    uint32_t nids[4];
    void **out_ptrs[4];
    int i;

    nids[0] = NID_CELL_USBD_INIT;
    nids[1] = NID_CELL_USBD_OPEN_PIPE;
    nids[2] = NID_CELL_USBD_TRANSFER;
    nids[3] = NID_CELL_USBD_CLOSE_PIPE;

    out_ptrs[0] = out_init;
    out_ptrs[1] = out_open_pipe;
    out_ptrs[2] = out_transfer;
    out_ptrs[3] = out_close_pipe;

    /* Initialize match tracking */
    for (i = 0; i < 4; i++) {
        targets[i].nid = nids[i];
        targets[i].func_addr = NULL;
        targets[i].found = 0;
    }

    DEBUG_PRINT("[USB] Scanning game memory for cellUsbd NIDs...\n");

    /* Scan each memory region for each NID */
    for (r = 0; r < NUM_SCAN_REGIONS; r++) {
        uint32_t region_start = g_scan_regions[r][0];
        uint32_t region_size  = g_scan_regions[r][1];
        void    *region_ptr   = (void*)(uintptr_t)region_start;

        DEBUG_VERBOSE("[USB] Scanning region 0x%08X (size 0x%X)\n",
                     (unsigned)region_start, (unsigned)region_size);

        /* Skip NULL regions */
        if (region_ptr == NULL)
            continue;

        for (i = 0; i < 4; i++) {
            if (targets[i].found)
                continue;

            if (scan_for_nid(region_ptr, region_size,
                             targets[i].nid, &targets[i].func_addr) == 0) {
                targets[i].found = 1;
                DEBUG_PRINT("[USB] Found NID 0x%08X in region 0x%08X\n",
                           (unsigned)targets[i].nid,
                           (unsigned)region_start);
            }
        }

        /* Early exit if all found */
        all_found = 1;
        for (i = 0; i < 4; i++) {
            if (!targets[i].found) {
                all_found = 0;
                break;
            }
        }
        if (all_found)
            break;
    }

    /* Check results */
    all_found = 1;
    for (i = 0; i < 4; i++) {
        if (targets[i].found) {
            DEBUG_PRINT("[USB]  NID 0x%08X -> func_addr=%p\n",
                       (unsigned)targets[i].nid, targets[i].func_addr);
            if (out_ptrs[i])
                *out_ptrs[i] = targets[i].func_addr;
        } else {
            DEBUG_ERROR("[USB]  NID 0x%08X NOT FOUND in any region\n",
                       (unsigned)targets[i].nid);
            all_found = 0;
        }
    }

    if (!all_found) {
        DEBUG_ERROR("[USB] Some cellUsbd functions not located\n");

        /*
         * FALLBACK: If the 12-byte import table scan fails, try an
         * alternative scan looking for the actual import stub code
         * (lis r12, ... / ori r12, ... / lwz r12, 0(r12) / ...)
         * and use the lwz offset to locate the GOT slot.
         */
        return -1;
    }

    DEBUG_PRINT("[USB] All 4 cellUsbd functions located successfully\n");
    return 0;
}

/* ---------------------------------------------------------------
 * Installation & Shutdown
 * --------------------------------------------------------------- */

int usb_hook_init(void *prx_toc)
{
    void *target_init;
    void *target_open_pipe;
    void *target_transfer;
    void *target_close_pipe;
    hook_ctx_t ctx;
    int ret;

    /*
     * FIXED: The prx_toc parameter is accepted for compatibility with
     * the caller (main.c), but is no longer stored in hook_ctx_t.
     * TOC save/restore is now handled entirely by the preamble (hook.c)
     * and the inline asm TOC switching macros.
     */
    (void)prx_toc;

    if (g_usb_hooks.initialized) {
        DEBUG_PRINT("[USB] Hooks already initialized\n");
        return 0;
    }

    memset(&g_usb_hooks, 0, sizeof(g_usb_hooks));
    g_usb_hooks.next_pipe_id = 0x1000; /* Start handles above typical values */

    DEBUG_PRINT("[USB] Initializing USB hooks\n");

    /* ---------------------------------------------------------------
     * Step 1: Locate the game's cellUsbd function addresses via NID scan.
     * --------------------------------------------------------------- */
    ret = find_game_cellusbd_functions(&target_init,
                                        &target_open_pipe,
                                        &target_transfer,
                                        &target_close_pipe);
    if (ret != 0) {
        DEBUG_ERROR("[USB] Cannot install hooks without game function addresses\n");
        DEBUG_ERROR("[USB] NID scanner did not find all 4 function stubs\n");
        return -1;
    }

    /* ---------------------------------------------------------------
     * Step 2: Install detour hooks on each function.
     * --------------------------------------------------------------- */

    /* Hook cellUsbdInit */
    memset(&ctx, 0, sizeof(ctx));
    ctx.target_addr = target_init;
    ctx.hook_fn = (void*)my_cellUsbdInit;
    ctx.tramp = &g_usb_hooks.tramp_init;

    if (hook_install(&ctx) != 0) {
        DEBUG_ERROR("[USB] Failed to hook cellUsbdInit\n");
        return -1;
    }
    DEBUG_PRINT("[USB] cellUsbdInit hooked at %p\n", target_init);

    /* Hook cellUsbdOpenPipe */
    memset(&ctx, 0, sizeof(ctx));
    ctx.target_addr = target_open_pipe;
    ctx.hook_fn = (void*)my_cellUsbdOpenPipe;
    ctx.tramp = &g_usb_hooks.tramp_open_pipe;
    if (hook_install(&ctx) != 0) {
        DEBUG_ERROR("[USB] Failed to hook cellUsbdOpenPipe\n");
        return -1;
    }
    DEBUG_PRINT("[USB] cellUsbdOpenPipe hooked at %p\n", target_open_pipe);

    /* Hook cellUsbdTransfer */
    memset(&ctx, 0, sizeof(ctx));
    ctx.target_addr = target_transfer;
    ctx.hook_fn = (void*)my_cellUsbdTransfer;
    ctx.tramp = &g_usb_hooks.tramp_transfer;
    if (hook_install(&ctx) != 0) {
        DEBUG_ERROR("[USB] Failed to hook cellUsbdTransfer\n");
        return -1;
    }
    DEBUG_PRINT("[USB] cellUsbdTransfer hooked at %p\n", target_transfer);

    /* Hook cellUsbdClosePipe */
    memset(&ctx, 0, sizeof(ctx));
    ctx.target_addr = target_close_pipe;
    ctx.hook_fn = (void*)my_cellUsbdClosePipe;
    ctx.tramp = &g_usb_hooks.tramp_close_pipe;
    if (hook_install(&ctx) != 0) {
        DEBUG_ERROR("[USB] Failed to hook cellUsbdClosePipe\n");
        return -1;
    }
    DEBUG_PRINT("[USB] cellUsbdClosePipe hooked at %p\n", target_close_pipe);

    g_usb_hooks.initialized = 1;

    DEBUG_PRINT("[USB] All 4 USB hooks installed successfully\n");
    DEBUG_PRINT("[USB] Toy Pad emulation active (VID=0x%04X, PID=0x%04X)\n",
                TOYPAD_VID, TOYPAD_PID);

    return 0;
}

void usb_hook_shutdown(void)
{
    hook_ctx_t ctx;
    void *target_init;
    void *target_open_pipe;
    void *target_transfer;
    void *target_close_pipe;

    if (!g_usb_hooks.initialized) {
        return;
    }

    DEBUG_PRINT("[USB] Shutting down USB hooks\n");

    /* Restore original functions (requires addresses again). */
    if (find_game_cellusbd_functions(&target_init,
                                      &target_open_pipe,
                                      &target_transfer,
                                      &target_close_pipe) == 0) {
        /* Restore cellUsbdInit */
        memset(&ctx, 0, sizeof(ctx));
        ctx.target_addr = target_init;
        ctx.tramp = &g_usb_hooks.tramp_init;
        hook_remove(&ctx);

        /* Restore cellUsbdOpenPipe */
        memset(&ctx, 0, sizeof(ctx));
        ctx.target_addr = target_open_pipe;
        ctx.tramp = &g_usb_hooks.tramp_open_pipe;
        hook_remove(&ctx);

        /* Restore cellUsbdTransfer */
        memset(&ctx, 0, sizeof(ctx));
        ctx.target_addr = target_transfer;
        ctx.tramp = &g_usb_hooks.tramp_transfer;
        hook_remove(&ctx);

        /* Restore cellUsbdClosePipe */
        memset(&ctx, 0, sizeof(ctx));
        ctx.target_addr = target_close_pipe;
        ctx.tramp = &g_usb_hooks.tramp_close_pipe;
        hook_remove(&ctx);

        DEBUG_PRINT("[USB] All hooks removed\n");
    }

    /* Clear pipe pool. */
    memset(g_usb_hooks.pipes, 0, sizeof(g_usb_hooks.pipes));
    g_usb_hooks.toypad_claimed = 0;
    g_usb_hooks.initialized = 0;
}
