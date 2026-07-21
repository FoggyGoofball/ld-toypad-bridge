#!/usr/bin/env python3
"""Write refactored usb_hooks.c"""
import os

content = r'''/**
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
#include <cell/cell_fs.h>

#include "usb_hooks.h"
#include "network.h"
#include "debug.h"

/* Global state */
usb_hook_state_t g_usb_hooks;

#define CELL_OK                  0
#define CELL_USBD_ERROR_FAILED  -1

/* NID values for cellUsbd functions */
#define NID_CELL_USBD_INIT          0x7F5F00D3U
#define NID_CELL_USBD_OPEN_PIPE     0x1AB6D80BU
#define NID_CELL_USBD_TRANSFER      0x7B4436CEU
#define NID_CELL_USBD_CLOSE_PIPE    0x2F82F1A5U

#define NID_SCAN_CHUNK_SIZE  0x10000

static const uint32_t g_scan_regions[][2] = {
    { 0x00100000, 0x00800000 },
    { 0x01000000, 0x01000000 },
    { 0x02000000, 0x01000000 },
    { 0x30000000, 0x00800000 },
    { 0x40000000, 0x01000000 },
};
#define NUM_SCAN_REGIONS  (sizeof(g_scan_regions) / sizeof(g_scan_regions[0]))

/* Assembly wrapper declarations from toc_trampoline.s */
extern void wrapper_my_cellUsbdInit(void);
extern void wrapper_my_cellUsbdOpenPipe(void);
extern void wrapper_my_cellUsbdTransfer(void);
extern void wrapper_my_cellUsbdClosePipe(void);

/* Passthrough stub declarations from toc_trampoline.s */
extern int call_original_OpenPipe(uint32_t game_toc, void *tramp_addr,
    uint32_t *pipe_handle, uint32_t dev_id, void *ep_descriptor);
extern int call_original_Transfer(uint32_t game_toc, void *tramp_addr,
    uint32_t pipe_handle, void *buf, uint32_t *len,
    uint32_t arg4, uint32_t arg5);
extern int call_original_ClosePipe(uint32_t game_toc, void *tramp_addr,
    uint32_t pipe_handle);

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
    WRITE_ADDR_LINE("INIT_WRAP", wrapper_my_cellUsbdInit);
    WRITE_ADDR_LINE("OPENPIPE_ADDR", g_usb_hooks.target_open_pipe_addr);
    WRITE_ADDR_LINE("OPENPIPE_WRAP", wrapper_my_cellUsbdOpenPipe);
    WRITE_ADDR_LINE("TRANSFER_ADDR", g_usb_hooks.target_transfer_addr);
    WRITE_ADDR_LINE("TRANSFER_WRAP", wrapper_my_cellUsbdTransfer);
    WRITE_ADDR_LINE("CLOSEPIPE_ADDR", g_usb_hooks.target_close_pipe_addr);
    WRITE_ADDR_LINE("CLOSEPIPE_WRAP", wrapper_my_cellUsbdClosePipe);

#undef WRITE_ADDR_LINE

    buf[pos] = '\0';

    if (cellFsOpen("/dev_hdd0/tmp/ld_hooks_ready.txt",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) == CELL_OK) {
        cellFsWrite(fd, buf, pos, &written);
        cellFsClose(fd);
        DEBUG_PRINT("[USB] IPC file written (%d bytes)\n", pos);
        return 0;
    }
    DEBUG_ERROR("[USB] Failed to write IPC file\n");
    return -1;
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
 * HOOK: my_cellUsbdTransfer
 * ================================================================ */
int my_cellUsbdTransfer(uint32_t pipe_handle, void *buf,
                         uint32_t *len, uint32_t arg4, uint32_t arg5,
                         uint32_t game_toc)
{
    int toypad_type;
    (void)arg4; (void)arg5; (void)game_toc;

    toypad_type = usb_hook_is_toypad_pipe(pipe_handle);
    if (toypad_type == 0) {
        return call_original_Transfer(game_toc,
            (void*)(uintptr_t)g_usb_hooks.tramp_transfer_addr,
            pipe_handle, buf, len, arg4, arg5);
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
 * NID Scanner (unchanged logic from original)
 * ================================================================ */

typedef struct {
    uint32_t nid;
    void     *func_addr;
    int      found;
} nid_match_t;

static int scan_for_nid(void *start, uint32_t size, uint32_t target_nid,
                         void **out_addr)
{
    uint32_t *words = (uint32_t*)start;
    uint32_t nwords = size / sizeof(uint32_t);
    uint32_t i;

    for (i = 0; i <= nwords - 3; i += 3) {
        uint32_t nid      = words[i + 0];
        uint32_t reserved = words[i + 1];
        uint32_t got_ptr  = words[i + 2];

        if (nid != target_nid) continue;
        if (reserved != 0 && reserved > 0x1000) continue;
        if (got_ptr == 0 || got_ptr < 0x00010000 || got_ptr > 0x4FFFFFFF) continue;

        /* Dereference GOT slot */
        uint32_t *got_slot = (uint32_t*)(uintptr_t)got_ptr;
        uint32_t func_addr = *got_slot;
        if (func_addr == 0 || func_addr < 0x00010000) continue;

        *out_addr = (void*)(uintptr_t)func_addr;
        return 0;
    }
    return -1;
}

static int find_game_cellusbd_functions(void **out_init,
    void **out_open_pipe, void **out_transfer, void **out_close_pipe)
{
    nid_match_t targets[4];
    unsigned int r;
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

    for (i = 0; i < 4; i++) {
        targets[i].nid = nids[i];
        targets[i].func_addr = NULL;
        targets[i].found = 0;
    }

    for (r = 0; r < NUM_SCAN_REGIONS; r++) {
        uint32_t region_start = g_scan_regions[r][0];
        uint32_t region_size  = g_scan_regions[r][1];
        void    *region_ptr   = (void*)(uintptr_t)region_start;
        if (region_ptr == NULL) continue;
        for (i = 0; i < 4; i++) {
            if (targets[i].found) continue;
            if (scan_for_nid(region_ptr, region_size,
                             targets[i].nid, &targets[i].func_addr) == 0)
                targets[i].found = 1;
        }
        all_found = 1;
        for (i = 0; i < 4; i++) { if (!targets[i].found) { all_found = 0; break; } }
        if (all_found) break;
    }

    all_found = 1;
    for (i = 0; i < 4; i++) {
        if (targets[i].found) {
            if (out_ptrs[i]) *out_ptrs[i] = targets[i].func_addr;
        } else {
            all_found = 0;
        }
    }
    return all_found ? 0 : -1;
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

    /* Step 1: Find function addresses via NID scan */
    ret = find_game_cellusbd_functions(&target_init, &target_open_pipe,
                                        &target_transfer, &target_close_pipe);
    if (ret != 0) {
        DEBUG_ERROR("[USB] NID scan failed\n");
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
      if (cellFsOpen("/dev_hdd0/tmp/ld_hooks_shutdown.txt",
                     CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                     &fd, NULL, 0) == CELL_OK) {
          cellFsWrite(fd, "STATUS=shutdown\n", 15, &written);
          cellFsClose(fd);
      }
    }

    memset(g_usb_hooks.pipes, 0, sizeof(g_usb_hooks.pipes));
    g_usb_hooks.toypad_claimed = 0;
    g_usb_hooks.initialized = 0;
    DEBUG_PRINT("[USB] Shutdown complete\n");
}
'''

os.chdir(os.path.dirname(os.path.abspath(__file__)))
with open('usb_hooks.c', 'w') as f:
    f.write(content)
print(f"usb_hooks.c written ({len(content)} bytes)")
