/**
 * toc_trampoline_c.c — Dynamic OPD Wrappers (Expert Guided Fix)
 *
 * PURPOSE:
 *   The Sony SDK's ppu-lv2-prx-fixup requires OPD entries for every
 *   cross-object function call. Assembly (.s) symbols are pure .text
 *   without OPDs, causing "undefined reference" linker errors when
 *   declared as extern function prototypes.
 *
 *   Expert-recommended solution: declare assembly symbols as extern
 *   uint32_t (data objects, not functions), manually construct OPD
 *   structs in C data sections, then cast to function pointers.
 *   This completely hides the assembly-to-C boundary from prx-fixup.
 *
 * ARCHITECTURE:
 *   asm_wrapper_*    — Raw .text addresses (for preamble bctr targets)
 *                      No OPD needed (preamble branches via ctr).
 *                      Addresses exported via get_wrapper_*_addr().
 *
 *   asm_passthrough_* — Raw .text passthrough stubs. OPDs constructed
 *                      in C via ppc_opd_t structs. Called from C hooks
 *                      through OPD function pointers.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 1. Assembly Symbols (declared as uint32_t data, NOT functions!)
 *
 *    Declaring as extern uint32_t tells the linker these are plain
 *    data symbols. No OPD is generated or expected. The address
 *    resolves directly to the .text code location.
 * ================================================================ */

/* Hook wrapper symbols (called directly from preamble via bctr) */
extern uint32_t asm_wrapper_my_cellUsbdInit;
extern uint32_t asm_wrapper_my_cellUsbdOpenPipe;
extern uint32_t asm_wrapper_my_cellUsbdTransfer;
extern uint32_t asm_wrapper_my_cellUsbdClosePipe;

/* Passthrough stub symbols (OPDs constructed below) */
extern uint32_t asm_passthrough_OpenPipe;
extern uint32_t asm_passthrough_Transfer;
extern uint32_t asm_passthrough_ClosePipe;

/* ================================================================
 * 2. CellOS OPD Structure
 *
 *    PowerPC 32-bit ABI on CellOS: function pointers point to
 *    a 3-word OPD struct in the .opd section.
 * ================================================================ */

typedef struct {
    uint32_t code_addr;    /* Ptr to .text code */
    uint32_t toc_addr;     /* TOC base value (loaded into r2 on call) */
    uint32_t env_ptr;      /* Environment pointer (unused, set to 0) */
} ppc_opd_t;

/* ================================================================
 * 3. Manually-constructed OPDs for passthrough stubs
 *
 *    toc_addr = 0: The passthrough stub immediately overwrites r2
 *    with the game_toc passed in r3, so the OPD-loaded TOC value
 *    is irrelevant.
 * ================================================================ */

/* Use uintptr_t for pointer-to-integer conversion to suppress truncation warnings */
ppc_opd_t g_opd_passthrough_OpenPipe = {
    .code_addr = (uint32_t)(uintptr_t)&asm_passthrough_OpenPipe,
    .toc_addr  = 0,
    .env_ptr   = 0
};

ppc_opd_t g_opd_passthrough_Transfer = {
    .code_addr = (uint32_t)(uintptr_t)&asm_passthrough_Transfer,
    .toc_addr  = 0,
    .env_ptr   = 0
};

ppc_opd_t g_opd_passthrough_ClosePipe = {
    .code_addr = (uint32_t)(uintptr_t)&asm_passthrough_ClosePipe,
    .toc_addr  = 0,
    .env_ptr   = 0
};

/* ================================================================
 * 4. Function Pointer Types for Passthrough Stubs
 *
 *    First two args (game_toc, tramp_addr) are consumed by the
 *    assembly stub before shifting remaining args down.
 * ================================================================ */

typedef int (*passthrough_openpipe_fn)(
    uint32_t game_toc,
    void    *tramp_addr,
    uint32_t *pipe_handle,
    uint32_t dev_id,
    void    *ep_descriptor
);

typedef int (*passthrough_transfer_fn)(
    uint32_t game_toc,
    void    *tramp_addr,
    uint32_t pipe_handle,
    void    *buf,
    uint32_t *len,
    uint32_t arg4,
    uint32_t arg5
);

typedef int (*passthrough_close_fn)(
    uint32_t game_toc,
    void    *tramp_addr,
    uint32_t pipe_handle
);

/* ================================================================
 * 5. Public Function Pointers (used by usb_hooks.c)
 *
 *    Cast the OPD struct address to function pointer type.
 *    PowerPC calling convention: calling through OPD loads the
 *    target's TOC into r2, but our stub overwrites it immediately
 *    with the game_toc passed in r3.
 * ================================================================ */

passthrough_openpipe_fn call_original_OpenPipe =
    (passthrough_openpipe_fn)&g_opd_passthrough_OpenPipe;

passthrough_transfer_fn call_original_Transfer =
    (passthrough_transfer_fn)&g_opd_passthrough_Transfer;

passthrough_close_fn call_original_ClosePipe =
    (passthrough_close_fn)&g_opd_passthrough_ClosePipe;

/* ================================================================
 * 6. Wrapper Address Helpers (for IPC file)
 *
 *    These return the raw .text addresses that Node.js uses as
 *    the target in the 4-instruction preamble (lis/ori/mtctr/bctr).
 * ================================================================ */

uint32_t get_wrapper_init_addr(void)
{
    return (uint32_t)&asm_wrapper_my_cellUsbdInit;
}

uint32_t get_wrapper_open_pipe_addr(void)
{
    return (uint32_t)&asm_wrapper_my_cellUsbdOpenPipe;
}

uint32_t get_wrapper_transfer_addr(void)
{
    return (uint32_t)&asm_wrapper_my_cellUsbdTransfer;
}

uint32_t get_wrapper_close_pipe_addr(void)
{
    return (uint32_t)&asm_wrapper_my_cellUsbdClosePipe;
}

#ifdef __cplusplus
}
#endif
