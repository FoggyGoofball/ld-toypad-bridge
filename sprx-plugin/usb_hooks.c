/**
 * usb_hooks.c — REFACTORED 2026-07-20
 *
 * CRITICAL CHANGES:
 *   1. Removed #include "hook.h" — hook_install/remove are gone
 *   2. Added sys_memory_allocate for executable trampoline pages
 *   3. Updated function signatures: game_toc is LAST argument
 *   4. Passthrough uses call_original_* assembly stubs (from toc_trampoline.s)
 *   5. No more inline asm mr r2 switching (ABI violation fix)
 *   6. usb_hook_init writes IPC file for Node.js orchestrator
 *
 * Architecture:
 *   wrapper_* (asm) -> my_cellUsbd* (C) -> return or call_original_* (asm)
 *   TOC management entirely in toc_trampoline.s assembly
 */

#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/memory.h>
#include <sys/timer.h>
#include <cell/cell_fs.h>

/* Sony SDK: sys_memory_allocate is in <sys/memory.h>
 * but SYS_MEMORY_CONTAINER_DEFAULT may not be declared.
 * 0xFFFFFFFF is the default container ID on CellOS. */
#define SYS_MEMORY_CONTAINER_DEFAULT  ((uint32_t)0xFFFFFFFFu)

#include "usb_hooks.h"
#include "network.h"
#include "debug.h"

/* Global state */
usb_hook_state_t g_usb_hooks;

#define CELL_OK                  0
#define CELL_USBD_ERROR_FAILED  -1

/* cellUsbd function imports from -lusbd_stub.
 * We only take addresses for OPD extraction — never called directly. */
extern int cellUsbdInit(void);
extern int cellUsbdOpenPipe(void *pipe_handle, uint32_t dev_id, void *ep_descriptor);
extern int cellUsbdInterruptTransfer(uint32_t pipe_handle, void *buf, uint32_t *len,
                                     void *done_cb, void *arg);
extern int cellUsbdClosePipe(uint32_t pipe_handle);

/* CellOS OPD (Official Procedure Descriptor) structure.
 * On PowerPC 32-bit CellOS, function pointers point to a 12-byte OPD
 * struct containing: code address, TOC address, environment pointer.
 * We extract the code_addr from our own SPRX's resolved imports to
 * get the real function addresses without scanning the game's memory. */
typedef struct {
    uint32_t code_addr;    /* Ptr to .text code */
    uint32_t toc_addr;     /* TOC base value (loaded into r2 on call) */
    uint32_t env_ptr;      /* Environment pointer (unused, set to 0) */
} ppc_opd_t;

/* OPD-based function pointers from toc_trampoline_c.c */
extern int (*call_original_OpenPipe)(uint32_t game_toc, void *tramp_addr,
    uint32_t *pipe_handle, uint32_t dev_id, void *ep_descriptor);
extern int (*call_original_Transfer)(uint32_t game_toc, void *tramp_addr,
    uint32_t pipe_handle, void *buf, uint32_t *len,
    void *done_cb, void *arg);
extern int (*call_original_ClosePipe)(uint32_t game_toc, void *tramp_addr,
    uint32_t pipe_handle);

/* Wrapper address helpers from toc_trampoline_c.c */
extern uint32_t get_wrapper_init_addr(void);
extern uint32_t get_wrapper_open_pipe_addr(void);
extern uint32_t get_wrapper_transfer_addr(void);
extern uint32_t get_wrapper_close_pipe_addr(void);

/* ================================================================
 * Trampoline Allocation (sys_memory_allocate R-W-X)
 * ================================================================
 * We allocate 64KB executable page. Each trampoline = 32 bytes (8 insns).
 * Four trampolines fit easily in 64KB. We use sys_memory_allocate
 * which returns R-W-X pages suitable for executable code.
 * ================================================================ */
#define TRAMPOLINE_PAGE_SIZE  (64 * 1024)
#define TRAMPOLINE_BLOCK_SIZE 32

static int allocate_trampolines(void)
{
    sys_memory_container_t container;
    uint32_t base_addr;
    int ret;

    container = SYS_MEMORY_CONTAINER_DEFAULT;

    ret = sys_memory_allocate(TRAMPOLINE_PAGE_SIZE,
                               SYS_MEMORY_PAGE_SIZE_64K,
                               &base_addr);
    if (ret != 0) {
        DEBUG_ERROR("[USB] sys_memory_allocate failed: 0x%x\n", ret);
        return -1;
    }

    g_usb_hooks.tramp_init_addr        = base_addr;
    g_usb_hooks.tramp_open_pipe_addr   = base_addr + TRAMPOLINE_BLOCK_SIZE;
    g_usb_hooks.tramp_transfer_addr    = base_addr + 2 * TRAMPOLINE_BLOCK_SIZE;
    g_usb_hooks.tramp_close_pipe_addr  = base_addr + 3 * TRAMPOLINE_BLOCK_SIZE;

    /* Heartbeat counter at offset 128 (after 4 × 32-byte trampolines).
     * Zero-initialized by sys_memory_allocate (page is zeroed).
     * Incremented each worker loop iteration at ~20 Hz.
     * Polled by Node.js orchestrator via PS3MAPI /read_process. */
    g_usb_hooks.heartbeat = (volatile uint32_t*)(uintptr_t)(base_addr + 128);
    DEBUG_PRINT("[USB] Heartbeat counter at 0x%08X\n",
                (unsigned)(base_addr + 128));

    DEBUG_PRINT("[USB] Trampoline page at 0x%08X (size=%u)\n",
                (unsigned)base_addr, (unsigned)TRAMPOLINE_PAGE_SIZE);
    return 0;
}

/* ================================================================
 * IPC File Helpers (HDD-based, for Node.js orchestrator)
 *
 * The SPRX writes resolved addresses to /dev_hdd0/tmp/ld_hooks_ready.txt.
 * Node.js polls via HTTP GET download.ps3?file=...,
 * then writes preamble via /write_process (Ring 0).
 * ================================================================ */

static int write_ipc_file(void)
{
    int fd;
    uint64_t written;
    char buf[512];
    int pos = 0;
    int started, shift, nib;
    uint32_t v;
    const char* s;

    s = "STATUS=ready\n"; while (*s) buf[pos++] = *s++;
    s = "TRAMP_BASE=0x"; while (*s) buf[pos++] = *s++;
    v = g_usb_hooks.tramp_init_addr;
    started = 0;
    for (shift = 28; shift >= 0; shift -= 4) {
        nib = (v >> shift) & 0xF;
        if (nib || started || shift == 0) {
            started = 1;
            buf[pos++] = nib <= 9 ? (char)('0' + nib) : (char)('A' + nib - 10);
        }
    }
    buf[pos++] = '\n';

/* Macro: write KEY=0xVALUE\n */
#define WRITE_ADDR_LINE(key, val) do { \
    const char* k = (key); \
    while (*k) buf[pos++] = *k++; \
    buf[pos++] = '='; buf[pos++] = '0'; buf[pos++] = 'x'; \
    v = (uint32_t)(uintptr_t)(val); \
    started = 0; \
    for (shift = 28; shift >= 0; shift -= 4) { \
        nib = (v >> shift) & 0xF; \
        if (nib || started || shift == 0) { \
            started = 1; \
            buf[pos++] = nib <= 9 ? (char)('0' + nib) : (char)('A' + nib - 10); \
        } \
    } \
    buf[pos++] = '\n'; \
} while(0)

    WRITE_ADDR_LINE("INIT_ADDR", g_usb_hooks.target_init_addr);
    WRITE_ADDR_LINE("INIT_WRAP", get_wrapper_init_addr());
    WRITE_ADDR_LINE("OPENPIPE_ADDR", g_usb_hooks.target_open_pipe_addr);
    WRITE_ADDR_LINE("OPENPIPE_WRAP", get_wrapper_open_pipe_addr());
    WRITE_ADDR_LINE("TRANSFER_ADDR", g_usb_hooks.target_transfer_addr);
    WRITE_ADDR_LINE("TRANSFER_WRAP", get_wrapper_transfer_addr());
    WRITE_ADDR_LINE("CLOSEPIPE_ADDR", g_usb_hooks.target_close_pipe_addr);
    WRITE_ADDR_LINE("CLOSEPIPE_WRAP", get_wrapper_close_pipe_addr());

#undef WRITE_ADDR_LINE

    buf[pos] = '\0';

    /* Write to .tmp file first, then atomic rename.
     *
     * CRITICAL: CELL_FS_O_TRUNC clears the inode instantaneously.
     * If Node.js issues an HTTP download.ps3?file=... request during
     * the write, it will parse a truncated file (partial lines or
     * missing addresses). By writing to ld_hooks.tmp first and then
     * calling cellFsRename(), the orchestrator always sees either the
     * complete old file (if rename hasn't executed) or the complete
     * new file (if rename has executed) — never a partial write. */
    if (cellFsOpen("/dev_hdd0/tmp/ld_hooks.tmp",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_OK) {
        DEBUG_ERROR("[USB] Failed to open temp IPC file\n");
        return -1;
    }
    cellFsWrite(fd, buf, pos, &written);
    cellFsClose(fd);

    /* Atomic rename — if this crashes, Node.js sees a stale file
     * (still valid format, just out-of-date content), never a
     * truncated one. */
    if (cellFsRename("/dev_hdd0/tmp/ld_hooks.tmp",
                     "/dev_hdd0/tmp/ld_hooks_ready.txt") != CELL_OK) {
        DEBUG_ERROR("[USB] cellFsRename for ready file failed\n");
        return -1;
    }

    DEBUG_PRINT("[USB] IPC file written (%d bytes, atomic rename)\n", pos);
    return 0;
}

/* ---- Pipe tracking (unchanged from original) ---- */

int usb_hook_is_toypad_pipe(uint32_t pipe_handle)
{
    int i;
    for (i = 0; i < USB_HOOK_MAX_PIPES; i++) {
        if (g_usb_hooks.pipes[i].in_use &&
            g_usb_hooks.pipes[i].pipe_handle == pipe_handle) {
            if (g_usb_hooks.pipes[i].ep_addr == TOYPAD_EP_IN) return 1;
            if (g_usb_hooks.pipes[i].ep_addr == TOYPAD_EP_OUT) return 2;
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

static usb_hook_pipe_t *alloc_pipe(void)
{
    int i;
    uint32_t handle = ++g_usb_hooks.next_pipe_id;
    if (handle == 0) handle = ++g_usb_hooks.next_pipe_id;
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

static void free_pipe(uint32_t pipe_handle)
{
    usb_hook_pipe_t *pipe = usb_hook_lookup_pipe(pipe_handle);
    if (pipe) {
        memset(pipe, 0, sizeof(*pipe));
    }
}

static uint8_t extract_ep_addr(const void *ep_descriptor)
{
    if (ep_descriptor == NULL) return 0;
    return ((const uint8_t *)ep_descriptor)[2];
}

/* ================================================================
 * HOOK: my_cellUsbdInit
 * ================================================================ */
int my_cellUsbdInit(uint32_t game_toc)
{
    (void)game_toc;
    DEBUG_PRINT("[USB] cellUsbdInit() intercepted returning CELL_OK\n");
    return CELL_OK;
}

/* ================================================================
 * HOOK: my_cellUsbdOpenPipe
 * ================================================================ */
int my_cellUsbdOpenPipe(uint32_t *pipe_handle, uint32_t dev_id,
                         void *ep_descriptor, uint32_t game_toc)
{
    uint8_t ep_addr;
    usb_hook_pipe_t *pipe;

    if (pipe_handle == NULL) {
        return CELL_USBD_ERROR_FAILED;
    }

    ep_addr = extract_ep_addr(ep_descriptor);

    if (ep_addr == TOYPAD_EP_IN || ep_addr == TOYPAD_EP_OUT) {
        pipe = alloc_pipe();
        if (pipe == NULL) return CELL_USBD_ERROR_FAILED;
        pipe->dev_id = dev_id;
        pipe->ep_addr = ep_addr;
        *pipe_handle = pipe->pipe_handle;
        g_usb_hooks.toypad_claimed = 1;
        return CELL_OK;
    }

    /* Non-ToyPad: passthrough via assembly stub (handles TOC restore) */
    return call_original_OpenPipe(game_toc,
        (void*)(uintptr_t)g_usb_hooks.tramp_open_pipe_addr,
        pipe_handle, dev_id, ep_descriptor);
}

/* ================================================================
 * HOOK: my_cellUsbdInterruptTransfer
 * ================================================================ */
int my_cellUsbdInterruptTransfer(uint32_t pipe_handle, void *buf,
                                  uint32_t *len, void *done_cb, void *arg,
                                  uint32_t game_toc)
{
    int toypad_type;
    (void)done_cb; (void)arg; (void)game_toc;

    toypad_type = usb_hook_is_toypad_pipe(pipe_handle);
    if (toypad_type == 0) {
        return call_original_Transfer(game_toc,
            (void*)(uintptr_t)g_usb_hooks.tramp_transfer_addr,
            pipe_handle, buf, len, done_cb, arg);
    }

    if (toypad_type == 1) {
        uint32_t max_len;
        uint8_t response[512];
        uint8_t zone = 1;
        uint8_t seq;
        int recv_len;
        if (buf == NULL || len == NULL) return CELL_USBD_ERROR_FAILED;
        max_len = *len;
        if (max_len > 256) max_len = 256;
        seq = (uint8_t)(g_usb_hooks.next_pipe_id++);
        if (network_send_poll(zone, seq) < 0) {
            if (max_len > 0) memset(buf, 0, max_len);
            *len = 0;
            return CELL_OK;
        }
        recv_len = network_recv(response, sizeof(response));
        if (recv_len > 2 && response[0] == 0x00) {
            int payload_len = recv_len - 3;
            if (payload_len > (int)max_len) payload_len = (int)max_len;
            if (payload_len > 0) memcpy(buf, response + 3, (size_t)payload_len);
            *len = (uint32_t)payload_len;
        } else {
            if (max_len > 0) {
                memset(buf, 0, max_len);
                ((uint8_t*)buf)[0] = 0x01;
            }
            *len = max_len;
        }
        return CELL_OK;
    }

    if (toypad_type == 2) {
        uint8_t zone = 1;
        uint8_t seq = (uint8_t)(g_usb_hooks.next_pipe_id++);
        if (buf == NULL || len == NULL) return CELL_USBD_ERROR_FAILED;
        network_send_data(zone, seq, (const uint8_t*)buf, (int)*len);
        return CELL_OK;
    }

    return CELL_USBD_ERROR_FAILED;
}

/* ================================================================
 * HOOK: my_cellUsbdClosePipe
 * ================================================================ */
int my_cellUsbdClosePipe(uint32_t pipe_handle, uint32_t game_toc)
{
    int toypad_type = usb_hook_is_toypad_pipe(pipe_handle);
    if (toypad_type == 0) {
        return call_original_ClosePipe(game_toc,
            (void*)(uintptr_t)g_usb_hooks.tramp_close_pipe_addr,
            pipe_handle);
    }
    free_pipe(pipe_handle);
    return CELL_OK;
}

/* ================================================================
 * OPD Extraction (Replaces NID Scanner)
 *
 * Instead of scanning the game's memory for NID stubs — which fails
 * because CellOS lazy binding means GOT entries are PLT stubs until
 * the game actually calls cellUsbd — we use the OPD trick.
 *
 * When the SPRX is loaded with -lusbd_stub, the CellOS PRX loader
 * resolves the cellUsbd imports for OUR module. By casting each
 * imported function pointer to a ppc_opd_t* and reading the code_addr
 * field, we get the real resolved address in the system PRX region
 * (≥ 0x30000000). No scanning, no retries, no chicken-and-egg.
 *
 * EXPERT REFERENCE:
 *   "In PowerPC64 (and CellOS 32-bit pointers acting under similar ABI),
 *    a C function pointer is actually a pointer to an Official Procedure
 *    Descriptor (OPD). You can extract the resolved code address directly
 *    from your own SPRX's runtime environment."
 * ================================================================ */

static int find_cellusbd_functions_via_opd(
    void **out_init, void **out_open_pipe,
    void **out_transfer, void **out_close_pipe)
{
    const ppc_opd_t *opd;
    uint32_t code_addr;

    if (out_init == NULL || out_open_pipe == NULL ||
        out_transfer == NULL || out_close_pipe == NULL) {
        return -1;
    }

    /* cellUsbdInit */
    opd = (const ppc_opd_t*)(uintptr_t)cellUsbdInit;
    code_addr = opd->code_addr;
    DEBUG_PRINT("[USB] OPD: cellUsbdInit => code=0x%08X (opd at 0x%08X)\n",
                (unsigned)code_addr, (unsigned)(uintptr_t)opd);
    if (code_addr < 0x30000000 || code_addr > 0x4FFFFFFF) {
        DEBUG_ERROR("[USB] OPD: cellUsbdInit code_addr out of range (0x%08X)\n",
                    (unsigned)code_addr);
        return -1;
    }
    *out_init = (void*)(uintptr_t)code_addr;

    /* cellUsbdOpenPipe */
    opd = (const ppc_opd_t*)(uintptr_t)cellUsbdOpenPipe;
    code_addr = opd->code_addr;
    DEBUG_PRINT("[USB] OPD: cellUsbdOpenPipe => code=0x%08X\n", (unsigned)code_addr);
    if (code_addr < 0x30000000 || code_addr > 0x4FFFFFFF) {
        DEBUG_ERROR("[USB] OPD: cellUsbdOpenPipe code_addr out of range (0x%08X)\n",
                    (unsigned)code_addr);
        return -1;
    }
    *out_open_pipe = (void*)(uintptr_t)code_addr;

    /* cellUsbdInterruptTransfer */
    opd = (const ppc_opd_t*)(uintptr_t)cellUsbdInterruptTransfer;
    code_addr = opd->code_addr;
    DEBUG_PRINT("[USB] OPD: cellUsbdTransfer => code=0x%08X\n", (unsigned)code_addr);
    if (code_addr < 0x30000000 || code_addr > 0x4FFFFFFF) {
        DEBUG_ERROR("[USB] OPD: cellUsbdTransfer code_addr out of range (0x%08X)\n",
                    (unsigned)code_addr);
        return -1;
    }
    *out_transfer = (void*)(uintptr_t)code_addr;

    /* cellUsbdClosePipe */
    opd = (const ppc_opd_t*)(uintptr_t)cellUsbdClosePipe;
    code_addr = opd->code_addr;
    DEBUG_PRINT("[USB] OPD: cellUsbdClosePipe => code=0x%08X\n", (unsigned)code_addr);
    if (code_addr < 0x30000000 || code_addr > 0x4FFFFFFF) {
        DEBUG_ERROR("[USB] OPD: cellUsbdClosePipe code_addr out of range (0x%08X)\n",
                    (unsigned)code_addr);
        return -1;
    }
    *out_close_pipe = (void*)(uintptr_t)code_addr;

    DEBUG_PRINT("[USB] All 4 cellUsbd functions resolved via OPD\n");
    return 0;
}

/* ================================================================
 * usb_hook_init — REFACTORED
 * ================================================================ */

int usb_hook_init(void)
{
    void *target_init;
    void *target_open_pipe;
    void *target_transfer;
    void *target_close_pipe;
    int ret;

    if (g_usb_hooks.initialized) return 0;
    memset(&g_usb_hooks, 0, sizeof(g_usb_hooks));
    g_usb_hooks.next_pipe_id = 0x1000;

    /* Step 1: Resolve target function addresses via OPD trick.
     *
     * Instead of scanning the game's memory for NID stubs (which fails
     * because CellOS lazy binding means GOT entries are PLT stubs until
     * the game actually calls cellUsbd), we extract the resolved code
     * addresses from our own SPRX's OPD import entries.
     *
     * The SPRX is linked with -lusbd_stub, so CellOS resolves cellUsbd
     * imports when our module is loaded. Casting the imported function
     * symbol to a ppc_opd_t* gives us the real code address directly.
     * No scanning, no retries, no chicken-and-egg problem. */
    ret = find_cellusbd_functions_via_opd(&target_init, &target_open_pipe,
                                           &target_transfer, &target_close_pipe);
    if (ret != 0) {
        DEBUG_ERROR("[USB] OPD extraction failed — SPRX imports not resolved\n");
        return -1;
    }

    g_usb_hooks.target_init_addr       = (uint32_t)(uintptr_t)target_init;
    g_usb_hooks.target_open_pipe_addr  = (uint32_t)(uintptr_t)target_open_pipe;
    g_usb_hooks.target_transfer_addr   = (uint32_t)(uintptr_t)target_transfer;
    g_usb_hooks.target_close_pipe_addr = (uint32_t)(uintptr_t)target_close_pipe;

    DEBUG_PRINT("[USB] Targets: init=0x%08X open=0x%08X transfer=0x%08X close=0x%08X\n",
        g_usb_hooks.target_init_addr, g_usb_hooks.target_open_pipe_addr,
        g_usb_hooks.target_transfer_addr, g_usb_hooks.target_close_pipe_addr);

    /* Step 2: Allocate executable trampoline pages */
    if (allocate_trampolines() != 0) {
        DEBUG_ERROR("[USB] Trampoline allocation failed\n");
        return -1;
    }

    /* Step 3: Copy original instructions + build branch-back in trampolines */
    { int ti;
      for (ti = 0; ti < 4; ti++) {
        uint32_t target_addr, tramp_addr;
        switch (ti) {
            case 0: target_addr = g_usb_hooks.target_init_addr;
                    tramp_addr = g_usb_hooks.tramp_init_addr; break;
            case 1: target_addr = g_usb_hooks.target_open_pipe_addr;
                    tramp_addr = g_usb_hooks.tramp_open_pipe_addr; break;
            case 2: target_addr = g_usb_hooks.target_transfer_addr;
                    tramp_addr = g_usb_hooks.tramp_transfer_addr; break;
            case 3: target_addr = g_usb_hooks.target_close_pipe_addr;
                    tramp_addr = g_usb_hooks.tramp_close_pipe_addr; break;
            default: continue;
        }
        uint32_t *tramp = (uint32_t*)(uintptr_t)tramp_addr;
        uint32_t *target = (uint32_t*)(uintptr_t)target_addr;
        int j;
        /* Copy 4 original instructions */
        for (j = 0; j < 4; j++) tramp[j] = target[j];
        /* Build branch-back to target+16: lis/ori/mtctr/bctr */
        uint32_t back_addr = target_addr + 16;
        tramp[4] = 0x3D600000 | ((back_addr >> 16) & 0xFFFF);
        tramp[5] = 0x616B0000 | (back_addr & 0xFFFF);
        tramp[6] = 0x7D6B03A6;
        tramp[7] = 0x4E800420;
      }
    }

    /* Step 4: Write IPC file for Node.js */
    write_ipc_file();

    g_usb_hooks.initialized = 1;
    DEBUG_PRINT("[USB] All 4 hooks prepared, awaiting Node.js preamble\n");
    return 0;
}

/* ================================================================
 * usb_hook_shutdown — REFACTORED
 * ================================================================ */
void usb_hook_shutdown(void)
{
    if (!g_usb_hooks.initialized) return;

    { int fd;
      uint64_t written;
      /* Atomic rename for shutdown IPC too */
      if (cellFsOpen("/dev_hdd0/tmp/ld_shutdown.tmp",
                     CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                     &fd, NULL, 0) == CELL_OK) {
          cellFsWrite(fd, "STATUS=shutdown\n", 15, &written);
          cellFsClose(fd);
          cellFsRename("/dev_hdd0/tmp/ld_shutdown.tmp",
                       "/dev_hdd0/tmp/ld_hooks_shutdown.txt");
      }
    }

    memset(g_usb_hooks.pipes, 0, sizeof(g_usb_hooks.pipes));
    g_usb_hooks.toypad_claimed = 0;
    g_usb_hooks.initialized = 0;
    DEBUG_PRINT("[USB] Shutdown complete\n");
}
