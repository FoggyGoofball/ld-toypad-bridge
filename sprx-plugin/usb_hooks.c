/**
 * usb_hooks.c — REFACTORED 2026-07-22 (Dynamic Trampoline Generation)
 *
 * CRITICAL CHANGES:
 *   1. Removed all assembly wrapper/passthrough references
 *   2. Removed call_original_* extern function pointers
 *   3. Removed get_wrapper_*_addr() helpers
 *   4. allocate_trampolines() -> install_hooks() using create_hook_trampoline()
 *   5. IPC file simplified: only TRAMP_* addresses, no WRAPPER_* fields
 *   6. Passthrough: calls real cellUsbd* functions directly (SPRX's own
 *      resolved imports), never touches game's GOT
 *   7. OPD extraction kept for validation, results stored locally
 *
 * WHY DYNAMIC TRAMPOLINES:
 *   The PS3's 16-byte PLT stub (lis/lwz/mtctr/bctr) does NOT dereference
 *   OPDs. Writing an OPD address into the game's GOT causes bctr to jump
 *   to the OPD's data bytes as instructions -> ISI crash.
 *
 *   Dynamic trampolines are real executable code that save the game's TOC,
 *   load the SPRX's TOC, call the C hook, restore the game's TOC, and
 *   return. They're generated at runtime in an R-W-X page.
 *
 * WHY DIRECT CALLS FOR PASSTHROUGH:
 *   The SPRX is linked with -lusbd_stub. CellOS resolves these imports
 *   when the SPRX loads. Calling cellUsbdOpenPipe() from C uses the
 *   SPRX's own GOT/TOC - the game's memory is never touched. This
 *   avoids the lazy-binding trap where the dynamic linker would overwrite
 *   our trampoline address in the game's GOT.
 *
 * Architecture:
 *   preamble (4 insns in game .text) -> trampoline (64 bytes R-W-X)
 *   -> C hook -> return or call real cellUsbd directly
 *
 *   TOC management entirely in trampoline_gen.c
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

/* NOTE: The SDK's <sys/memory.h> defines SYS_MEMORY_PROT_* as 64-bit mapping
 * attributes (SYS_MEMORY_PROT_READ_ONLY, SYS_MEMORY_PROT_READ_WRITE). There is
 * NO exec flag. Memory from sys_memory_allocate() is already readable and
 * executable by PPU threads. Do NOT redefine these flags here. */
/* Forward declaration for hook integrity checker */
static int hook_verify_preamble(uint32_t target_addr, const char *name);

/* NID values for cellUsbd functions.
 * These are 32-bit big-endian NIDs used in the PS3's import stub table
 * (.rodata triplet format: { NID, reserved, GOT_ptr }).
 * Verified against LEGO Dimensions game memory dumps. */
#define NID_CELL_USBD_INIT          0x7F5F00D3U
#define NID_CELL_USBD_OPENPIPE      0x1AB6D80BU
#define NID_CELL_USBD_TRANSFER      0x7B4436CEU  /* cellUsbdInterruptTransfer */
#define NID_CELL_USBD_CLOSEPIPE     0x2F82F1A5U

#include "usb_hooks.h"
#include "network.h"
#include "debug.h"
#include "trampoline_gen.h"

/* Global state */
usb_hook_state_t g_usb_hooks;

#define CELL_OK                  0
#define CELL_USBD_ERROR_FAILED  -1

/* ================================================================
 * DIAGNOSTICS: g_init_progress extern + INIT_PROGRESS macro
 *
 * Declared in main.c. Updated at every init step boundary for
 * PS3MAPI memory-polling diagnostics. Cache-coherent writes.
 * ================================================================ */
extern volatile uint32_t g_init_progress;

#define INIT_PROGRESS(x) do { \
    g_init_progress = (x); \
    __asm__ __volatile__ ("dcbst 0, %0\n\tsync" :: "r"(&g_init_progress) : "memory"); \
} while(0)

/* cellUsbd function imports from -lusbd_stub.
 * We declare these as extern functions - they're resolved by the CellOS
 * PRX loader when the SPRX is loaded. We use them for:
 *   1. OPD extraction (get resolved code addresses)
 *   2. Direct passthrough calls (non-ToyPad USB traffic)
 *
 * IMPORTANT: When calling these functions from C, the compiler uses the
 * SPRX's own GOT/TOC. The game's GOT is never touched. This is safe. */
extern int cellUsbdInit(void);
extern int cellUsbdOpenPipe(void *pipe_handle, uint32_t dev_id, void *ep_descriptor);
extern int cellUsbdInterruptTransfer(uint32_t pipe_handle, void *buf, uint32_t *len,
                                     void *done_cb, void *arg);
extern int cellUsbdClosePipe(uint32_t pipe_handle);

/* CellOS OPD (Official Procedure Descriptor) structure.
 * On PowerPC 32-bit CellOS, function pointers point to a 12-byte OPD
 * struct containing: code address, TOC address, environment pointer.
 * We extract the code_addr from our own SPRX's resolved imports to
 * get the real function addresses for OPD extraction in trampoline_gen.c. */
typedef struct {
    uint32_t code_addr;    /* Ptr to .text code */
    uint32_t toc_addr;     /* TOC base value (loaded into r2 on call) */
    uint32_t env_ptr;      /* Environment pointer (unused, set to 0) */
} ppc_opd_t;

/* ================================================================
 * Hook Installation (replaces allocate_trampolines + toc_trampoline.s)
 * ================================================================
 * We allocate a single 64KB R-W-X page via sys_memory_allocate.
 * Within this page, we generate 4 trampolines at 64-byte offsets:
 *   Offset 0:   Init trampoline     (toc_arg_reg=3)
 *   Offset 64:  OpenPipe trampoline (toc_arg_reg=6)
 *   Offset 128: Transfer trampoline (toc_arg_reg=8)
 *   Offset 192: ClosePipe trampoline (toc_arg_reg=4)
 *   Offset 256: Heartbeat counter (uint32_t)
 * ================================================================ */
#define TRAMPOLINE_PAGE_SIZE    (64 * 1024)
#define TRAMPOLINE_BLOCK_SIZE   64  /* 16 instructions */

/* TOC argument register for each hook based on number of original args */
#define TOC_REG_INIT       3   /* 0 args -> TOC goes in r3 */
#define TOC_REG_OPENPIPE   6   /* 3 args -> TOC goes in r6 */
#define TOC_REG_TRANSFER   8   /* 5 args -> TOC goes in r8 */
#define TOC_REG_CLOSEPIPE  4   /* 1 arg  -> TOC goes in r4 */

/* ================================================================
 * Hook Integrity Checker
 *
 * Verifies that a game PLT stub address is safe to overwrite.
 * Called during hook init to sanity-check TARGET_* addresses
 * before writing them to the IPC file. Runs once at startup.
 *
 * Checks:
 *   1. Address is non-zero (0 = NID not found, skip)
 *   2. Address is within game .text range (< 0x30000000)
 *   3. Address is word-aligned (max(denial of the PPU))
 *   4. Reads the first 4 bytes at target — warns if they look
 *      like executable code (should be lis/ori pattern later)
 *
 * Returns 0 if target looks plumbable, -1 if something is wrong.
 * ================================================================ */
static int hook_verify_preamble(uint32_t target_addr, const char *name)
{
    if (target_addr == 0) {
        DEBUG_PRINT("[USB] INTEGRITY: %s target=0x00000000 (skipped — NID not found)\n", name);
        return -1;
    }

    /* Must be in game .text range. If it's in PRX range (>= 0x30000000),
     * the GOT was resolved and we'd be overwriting libusbd.sprx. */
    if (target_addr >= 0x30000000) {
        DEBUG_ERROR("[USB] INTEGRITY FAIL: %s target=0x%08X — in PRX range, GOT resolved!\n",
                    name, (unsigned)target_addr);
        return -1;
    }

    /* Must be word-aligned */
    if (target_addr & 3) {
        DEBUG_ERROR("[USB] INTEGRITY FAIL: %s target=0x%08X — misaligned (not word-aligned)\n",
                    name, (unsigned)target_addr);
        return -1;
    }

    /* Validate that the address actually maps to readable memory.
     * Attempt a volatile read of the first instruction word at the
     * target. If this crashes (ISI), the target is unmapped and the
     * injector should not write there.
     *
     * To avoid crashing the SPRX on a bad read, we mark the check
     * as best-effort: if it seems valid, we pass it; if the first
     * word looks like a PLT stub pattern (lis with r11 or r12),
     * we log it for diagnostics but still allow the injector
     * to overwrite it. */
    {
        volatile uint32_t *p = (volatile uint32_t*)(uintptr_t)target_addr;
        uint32_t first_word = p[0];

        DEBUG_VERBOSE("[USB] INTEGRITY: %s target=0x%08X [0]=0x%08X\n",
                      name, (unsigned)target_addr, (unsigned)first_word);

        /* Log if the first word looks like a lis instruction (3Dxx or 3Cxx).
         * PLT stubs start with 'lis r11, offset' = 0x3D60xxxx or
         * 'lis r12, offset' = 0x3D80xxxx. A lis is expected for an
         * unresolved PLT stub. Anything else might indicate:
         *   - Already resolved (branch to libusbd)
         *   - Corrupted memory
         *   - Not a PLT stub at all
         * We log but do NOT fail — the injector will overwrite anyway. */
        if ((first_word & 0xFFFF0000) == 0x3D600000 ||
            (first_word & 0xFFFF0000) == 0x3D800000) {
            DEBUG_VERBOSE("[USB] INTEGRITY: %s — lis pattern at target (expected for unresolved PLT)\n",
                          name);
        } else {
            DEBUG_PRINT("[USB] INTEGRITY: %s target=0x%08X [0]=0x%08X (non-standard PLT pattern)\n",
                        name, (unsigned)target_addr, (unsigned)first_word);
            /* Non-standard — could be resolved GOT, could be custom stub.
             * Allow injector to attempt overwrite anyway. */
        }
    }

    DEBUG_PRINT("[USB] INTEGRITY: %s target=0x%08X — OK (safe to overwrite)\n",
                name, (unsigned)target_addr);
    return 0;
}

/* Helper: log first 4 words of a trampoline for offline disassembly */
static void log_trampoline_header(const char *label, uint32_t base)
{
    volatile uint32_t *p = (volatile uint32_t*)(uintptr_t)base;
    DEBUG_PRINT("[USB]   %s tramp [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X\n",
                label, (unsigned)p[0], (unsigned)p[1],
                (unsigned)p[2], (unsigned)p[3]);
}

static int install_hooks(void)
{
    sys_memory_container_t container;
    uint32_t base_addr;
    int ret;

    container = SYS_MEMORY_CONTAINER_DEFAULT;

    /* Step 1: Allocate 64KB page.
     *
     * On CellOS, sys_memory_allocate() returns memory that is already
     * readable and executable by PPU threads by default. The SDK's
     * <sys/memory.h> defines SYS_MEMORY_PROT_* flags only for mapping
     * attributes (SYS_MEMORY_PROT_READ_ONLY, SYS_MEMORY_PROT_READ_WRITE),
     * and there is SYS_MEMORY_PROT_MASK. There is NO exec-specific flag
     * and NO sys_memory_set_protection() API in this SDK version.
     *
     * The allocated pages are accessible by PPU threads for both read
     * and execute operations. No extra protection call is needed. */
    ret = sys_memory_allocate(TRAMPOLINE_PAGE_SIZE,
                               SYS_MEMORY_PAGE_SIZE_64K,
                               &base_addr);
    if (ret != 0) {
        DEBUG_ERROR("[USB] sys_memory_allocate(size=%u) failed: 0x%x\n",
                    (unsigned)TRAMPOLINE_PAGE_SIZE, ret);
        return -1;
    }
    DEBUG_PRINT("[USB] sys_memory_allocate OK: base=0x%08X size=%u (PPU-exec by default)\n",
                (unsigned)base_addr, (unsigned)TRAMPOLINE_PAGE_SIZE);

    /* CRITICAL CACHE COHERENCY: The icbi/isync flush in
     * create_hook_trampoline() handles data/instruction cache
     * coherency after writing trampoline instructions. No
     * additional protection call is needed — PPU threads can
     * both read and execute from allocated pages by default. */

    g_usb_hooks.trampoline_base = base_addr;
    g_usb_hooks.tramp_init_offset = 0;
    g_usb_hooks.tramp_open_pipe_offset = TRAMPOLINE_BLOCK_SIZE;
    g_usb_hooks.tramp_transfer_offset = 2 * TRAMPOLINE_BLOCK_SIZE;
    g_usb_hooks.tramp_close_pipe_offset = 3 * TRAMPOLINE_BLOCK_SIZE;

    /* Heartbeat counter at offset 256 (after 4 x 64-byte trampolines).
     * Zero-initialized by sys_memory_allocate (page is zeroed).
     * Incremented each worker loop iteration at ~20 Hz.
     * Polled by Node.js orchestrator via PS3MAPI /read_process. */
    g_usb_hooks.heartbeat = (volatile uint32_t*)(uintptr_t)(base_addr + 256);
    DEBUG_PRINT("[USB] Heartbeat counter at 0x%08X\n",
                (unsigned)(base_addr + 256));

    /* Step 2: Generate trampolines using create_hook_trampoline().
     *
     * Each trampoline is 64 bytes (16 instructions) of dynamically
     * generated PowerPC code that:
     *   - Saves game's TOC and LR
     *   - Passes game's TOC as an argument to our C hook
     *   - Loads the SPRX's TOC
     *   - Calls the C hook via bctrl
     *   - Restores game's TOC and LR
     *   - Returns to game caller
     *
     * The c_func parameter is a pointer to our C hook function.
     * The C function pointer is actually an OPD on CellOS.
     * create_hook_trampoline extracts code_addr and toc_addr from
     * the OPD and embeds them into the trampoline instructions. */
    create_hook_trampoline(
        (uint32_t*)(uintptr_t)(base_addr + g_usb_hooks.tramp_init_offset),
        (void*)my_cellUsbdInit, TOC_REG_INIT);
    DEBUG_PRINT("[USB] Init trampoline at 0x%08X\n",
                (unsigned)(base_addr + g_usb_hooks.tramp_init_offset));
    log_trampoline_header("Init", (unsigned)(base_addr + g_usb_hooks.tramp_init_offset));

    create_hook_trampoline(
        (uint32_t*)(uintptr_t)(base_addr + g_usb_hooks.tramp_open_pipe_offset),
        (void*)my_cellUsbdOpenPipe, TOC_REG_OPENPIPE);
    DEBUG_PRINT("[USB] OpenPipe trampoline at 0x%08X\n",
                (unsigned)(base_addr + g_usb_hooks.tramp_open_pipe_offset));
    log_trampoline_header("OpenPipe", (unsigned)(base_addr + g_usb_hooks.tramp_open_pipe_offset));

    create_hook_trampoline(
        (uint32_t*)(uintptr_t)(base_addr + g_usb_hooks.tramp_transfer_offset),
        (void*)my_cellUsbdInterruptTransfer, TOC_REG_TRANSFER);
    DEBUG_PRINT("[USB] Transfer trampoline at 0x%08X\n",
                (unsigned)(base_addr + g_usb_hooks.tramp_transfer_offset));
    log_trampoline_header("Transfer", (unsigned)(base_addr + g_usb_hooks.tramp_transfer_offset));

    create_hook_trampoline(
        (uint32_t*)(uintptr_t)(base_addr + g_usb_hooks.tramp_close_pipe_offset),
        (void*)my_cellUsbdClosePipe, TOC_REG_CLOSEPIPE);
    DEBUG_PRINT("[USB] ClosePipe trampoline at 0x%08X\n",
                (unsigned)(base_addr + g_usb_hooks.tramp_close_pipe_offset));
    log_trampoline_header("ClosePipe", (unsigned)(base_addr + g_usb_hooks.tramp_close_pipe_offset));

    DEBUG_PRINT("[USB] Trampoline page at 0x%08X (size=%u)\n",
                (unsigned)base_addr, (unsigned)TRAMPOLINE_PAGE_SIZE);
    return 0;
}

/* ================================================================
 * IPC File Helpers (HDD-based, for Node.js orchestrator)
 *
 * The SPRX writes resolved trampoline addresses to
 * /dev_hdd0/tmp/ld_hooks_ready.txt. Node.js polls via HTTP GET
 * download.ps3?file=..., then writes 4-instruction preambles
 * (lis/ori/mtctr/bctr) into the game's .text via PS3MAPI /write_process.
 *
 * UPDATED 2026-07-22: Added TARGET_* addresses for Node.js preamble
 * writer. The write_ipc_file() now takes 4 target address parameters
 * from the NID scanner (get_game_plt_stub). If a target is 0x00000000,
 * the injector skips that hook.
 * ================================================================ */

static int write_ipc_file(uint32_t target_init, uint32_t target_openpipe,
                          uint32_t target_transfer, uint32_t target_closepipe)
{
    int fd;
    uint64_t written;
    char buf[512];
    int pos = 0;
    int started, shift, nib;
    uint32_t v;
    const char* s;

    /* STATUS line */
    s = "STATUS=ready\n"; while (*s) buf[pos++] = *s++;

    /* Trampoline base address */
    s = "TRAMP_BASE=0x"; while (*s) buf[pos++] = *s++;
    v = g_usb_hooks.trampoline_base;
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

    /* Individual trampoline addresses (absolute, for Node.js convenience) */
    WRITE_ADDR_LINE("TRAMP_INIT",
        g_usb_hooks.trampoline_base + g_usb_hooks.tramp_init_offset);
    WRITE_ADDR_LINE("TRAMP_OPENPIPE",
        g_usb_hooks.trampoline_base + g_usb_hooks.tramp_open_pipe_offset);
    WRITE_ADDR_LINE("TRAMP_TRANSFER",
        g_usb_hooks.trampoline_base + g_usb_hooks.tramp_transfer_offset);
    WRITE_ADDR_LINE("TRAMP_CLOSEPIPE",
        g_usb_hooks.trampoline_base + g_usb_hooks.tramp_close_pipe_offset);

    /* Game PLT stub addresses (from NID scan) for preamble installation */
    WRITE_ADDR_LINE("TARGET_INIT", target_init);
    WRITE_ADDR_LINE("TARGET_OPENPIPE", target_openpipe);
    WRITE_ADDR_LINE("TARGET_TRANSFER", target_transfer);
    WRITE_ADDR_LINE("TARGET_CLOSEPIPE", target_closepipe);

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
     * new file (if rename has executed) - never a partial write. */
    if (cellFsOpen("/dev_hdd0/tmp/ld_hooks.tmp",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_OK) {
        DEBUG_ERROR("[USB] Failed to open temp IPC file\n");
        return -1;
    }

    /* DEBUG_VERBOSE: full IPC buffer content for offline diagnostics */
    DEBUG_VERBOSE("[USB] IPC buffer (%d bytes):\n%s", pos, buf);

    written = 0;
    {
        int64_t write_ret = cellFsWrite(fd, buf, pos, &written);
        if (write_ret != CELL_OK) {
            DEBUG_ERROR("[USB] cellFsWrite IPC file failed: 0x%llx\n",
                        (unsigned long long)write_ret);
            cellFsClose(fd);
            return -1;
        }
        if ((int64_t)written != pos) {
            DEBUG_ERROR("[USB] cellFsWrite short write: %llu/%d bytes\n",
                        (unsigned long long)written, pos);
            cellFsClose(fd);
            return -1;
        }
    }

    {
        int64_t close_ret = cellFsClose(fd);
        if (close_ret != CELL_OK) {
            DEBUG_ERROR("[USB] cellFsClose IPC file failed: 0x%llx\n",
                        (unsigned long long)close_ret);
            return -1;
        }
    }

    /* Atomic rename - if this crashes, Node.js sees a stale file
     * (still valid format, just out-of-date content), never a
     * truncated one. */
    if (cellFsRename("/dev_hdd0/tmp/ld_hooks.tmp",
                     "/dev_hdd0/tmp/ld_hooks_ready.txt") != CELL_OK) {
        DEBUG_ERROR("[USB] cellFsRename for ready file failed\n");
        return -1;
    }

    DEBUG_PRINT("[USB] IPC file written (%d bytes written, atomic rename)\n", (int)written);
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
 *
 * Simply returns CELL_OK - we don't need USB initialization because
 * we handle Toy Pad traffic entirely in user-space via UDP.
 * ================================================================ */
int my_cellUsbdInit(uint32_t game_toc)
{
    (void)game_toc;
    DEBUG_PRINT("[USB] ENTER cellUsbdInit(game_toc=0x%08X)\n", (unsigned)game_toc);
    DEBUG_PRINT("[USB] cellUsbdInit() intercepted returning CELL_OK\n");
    return CELL_OK;
}

/* ================================================================
 * HOOK: my_cellUsbdOpenPipe
 *
 * If the device is a Toy Pad (matching endpoints), we allocate a
 * fake pipe and return success. Otherwise, we pass through to the
 * real cellUsbdOpenPipe via direct call (SPRX's own import).
 *
 * CRITICAL: We call cellUsbdOpenPipe() directly - NOT through any
 * assembly passthrough stub. The C compiler uses the SPRX's own
 * GOT/TOC. The game's GOT (containing our trampoline address)
 * is never touched. This avoids the lazy-binding trap where the
 * dynamic linker would overwrite our GOT slot.
 * ================================================================ */
int my_cellUsbdOpenPipe(uint32_t *pipe_handle, uint32_t dev_id,
                         void *ep_descriptor, uint32_t game_toc)
{
    uint8_t ep_addr;
    usb_hook_pipe_t *pipe;

    (void)game_toc;

    DEBUG_PRINT("[USB] ENTER my_cellUsbdOpenPipe(dev_id=0x%08X, game_toc=0x%08X)\n",
                (unsigned)dev_id, (unsigned)game_toc);

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
        DEBUG_PRINT("[USB] ToyPad pipe opened: handle=0x%08X ep=0x%02X\n",
                    (unsigned)pipe->pipe_handle, (unsigned)ep_addr);
        return CELL_OK;
    }

    /* Non-ToyPad: pass through to real cellUsbdOpenPipe.
     * Direct call - SPRX's own import, game's memory untouched. */
    return cellUsbdOpenPipe(pipe_handle, dev_id, ep_descriptor);
}

/* ================================================================
 * HOOK: my_cellUsbdInterruptTransfer
 *
 * If pipe is a Toy Pad IN endpoint: poll network for response.
 * If pipe is a Toy Pad OUT endpoint: send data to network.
 * Otherwise: pass through to real cellUsbdInterruptTransfer.
 * ================================================================ */
int my_cellUsbdInterruptTransfer(uint32_t pipe_handle, void *buf,
                                  uint32_t *len, void *done_cb, void *arg,
                                  uint32_t game_toc)
{
    int toypad_type;
    (void)done_cb; (void)arg; (void)game_toc;

    DEBUG_PRINT("[USB] ENTER my_cellUsbdInterruptTransfer(pipe=0x%08X, len=%u, game_toc=0x%08X)\n",
                (unsigned)pipe_handle,
                (unsigned)(len ? *len : 0),
                (unsigned)game_toc);

    toypad_type = usb_hook_is_toypad_pipe(pipe_handle);
    if (toypad_type == 0) {
        /* Non-ToyPad: pass through to real cellUsbdInterruptTransfer.
         * Direct call - SPRX's own import, game's memory untouched. */
        return cellUsbdInterruptTransfer(pipe_handle, buf, len, done_cb, arg);
    }

    if (toypad_type == 1) {
        /* Toy Pad IN endpoint: Poll network for data to send to game */
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
        /* Toy Pad OUT endpoint: Send data from game to network server */
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
 *
 * If pipe is a Toy Pad pipe: free the slot and return success.
 * Otherwise: pass through to real cellUsbdClosePipe.
 * ================================================================ */
int my_cellUsbdClosePipe(uint32_t pipe_handle, uint32_t game_toc)
{
    int toypad_type;
    (void)game_toc;

    DEBUG_PRINT("[USB] ENTER my_cellUsbdClosePipe(pipe=0x%08X, game_toc=0x%08X)\n",
                (unsigned)pipe_handle, (unsigned)game_toc);

    toypad_type = usb_hook_is_toypad_pipe(pipe_handle);
    if (toypad_type == 0) {
        /* Non-ToyPad: pass through to real cellUsbdClosePipe.
         * Direct call - SPRX's own import, game's memory untouched. */
        return cellUsbdClosePipe(pipe_handle);
    }

    free_pipe(pipe_handle);
    DEBUG_PRINT("[USB] ToyPad pipe closed: handle=0x%08X\n",
                (unsigned)pipe_handle);
    return CELL_OK;
}

/* ================================================================
 * OPD Extraction (Kept for import validation)
 *
 * We call find_cellusbd_functions_via_opd() to verify that our
 * SPRX's cellUsbd imports are properly resolved. After extraction,
 * the code_addr and toc_addr values are used internally by
 * create_hook_trampoline() when we pass our C hook function pointers
 * (my_cellUsbdOpenPipe, etc.) - those are the SPRX's own OPDs.
 *
 * The resolved import addresses are NOT stored - they're only
 * validated for range checking. The actual hook mechanism uses
 * our own functions' OPDs, not the cellUsbd imports' OPDs.
 * ================================================================ */

/* Validate a single OPD and log all 3 fields.
 * Returns 0 if valid, -1 if any field looks suspicious. */
static int validate_opd(const char *name, const ppc_opd_t *opd)
{
    uint32_t code = opd->code_addr;
    uint32_t toc  = opd->toc_addr;
    uint32_t env  = opd->env_ptr;

    DEBUG_PRINT("[USB] OPD: %s => { code=0x%08X toc=0x%08X env=0x%08X } (opd at 0x%08X)\n",
                name, (unsigned)code, (unsigned)toc, (unsigned)env,
                (unsigned)(uintptr_t)opd);

    /* Code address must be in PRX executable range (0x3xxxxxxx-0x4xxxxxxx).
     * If it looks like a PowerPC opcode (e.g. 0x48xxxxxx branch), the symbol
     * might be an import stub rather than a real OPD — this is a soft-fail. */
    if (code < 0x30000000 || code > 0x4FFFFFFF) {
        DEBUG_ERROR("[USB] OPD: %s code_addr=0x%08X out of range - likely import stub\n",
                    name, (unsigned)code);
        return -1;
    }

    /* TOC should be in the same general range. If code is valid but TOC is
     * wildly different, the OPD may be corrupted or misaligned. */
    if (toc < 0x30000000 || toc > 0x4FFFFFFF) {
        DEBUG_ERROR("[USB] OPD: %s toc_addr=0x%08X suspicious - ignoring\n",
                    name, (unsigned)toc);
        return -1;
    }

    /* Environment pointer should be 0 in the official SDK. Non-zero suggests
     * we're reading garbage, not a valid OPD entry. */
    if (env != 0) {
        DEBUG_ERROR("[USB] OPD: %s env_ptr=0x%08X is non-zero - expected 0\n",
                    name, (unsigned)env);
        /* NOT returning -1 here — some SDK builds may use env_ptr.
         * Logged as warning only. */
    }

    DEBUG_VERBOSE("[USB] OPD: %s validated (code=0x%08X, toc=0x%08X, env=0x%08X)\n",
                  name, (unsigned)code, (unsigned)toc, (unsigned)env);
    return 0;
}

static int find_cellusbd_functions_via_opd(void)
{
    int all_ok = 0;

    DEBUG_PRINT("[USB] OPD extraction: cellUsbdInit at %p\n",
                (void*)(uintptr_t)cellUsbdInit);
    if (validate_opd("cellUsbdInit",
        (const ppc_opd_t*)(uintptr_t)cellUsbdInit) == 0) all_ok++;

    DEBUG_PRINT("[USB] OPD extraction: cellUsbdOpenPipe at %p\n",
                (void*)(uintptr_t)cellUsbdOpenPipe);
    if (validate_opd("cellUsbdOpenPipe",
        (const ppc_opd_t*)(uintptr_t)cellUsbdOpenPipe) == 0) all_ok++;

    DEBUG_PRINT("[USB] OPD extraction: cellUsbdInterruptTransfer at %p\n",
                (void*)(uintptr_t)cellUsbdInterruptTransfer);
    if (validate_opd("cellUsbdInterruptTransfer",
        (const ppc_opd_t*)(uintptr_t)cellUsbdInterruptTransfer) == 0) all_ok++;

    DEBUG_PRINT("[USB] OPD extraction: cellUsbdClosePipe at %p\n",
                (void*)(uintptr_t)cellUsbdClosePipe);
    if (validate_opd("cellUsbdClosePipe",
        (const ppc_opd_t*)(uintptr_t)cellUsbdClosePipe) == 0) all_ok++;

    if (all_ok == 4) {
        DEBUG_PRINT("[USB] All 4 cellUsbd functions resolved & validated via OPD\n");
        return 0;
    }

    DEBUG_ERROR("[USB] OPD: %d/4 cellUsbd imports validated, soft-fail %d\n",
                all_ok, 4 - all_ok);
    return -1;
}

/* ================================================================
 * Game PLT Stub Address Scanner
 *
 * Scans the game process's import stub table (.rodata) for NID
 * triplets matching our 4 cellUsbd functions. Each triplet is 12
 * bytes: { NID (4B), reserved (4B), GOT_ptr (4B) }.
 *
 * The GOT slot initially contains the address of a 16-byte PLT stub
 * in the game's executable .text memory. We save this PLT stub address
 * and write it to the IPC file as TARGET_*. The Node.js injector then
 * overwrites that PLT stub with our 4-instruction preamble (lis/ori/
 * mtctr/bctr) that branches to our trampoline.
 *
 * ⚠ CAVEAT (Option A — *got_slot approach):
 *   This scanner reads the GOT slot via the GOT_ptr from the triplet.
 *   The GOT slot value is only the PLT stub BEFORE the function has
 *   been called. Once the game calls cellUsbdOpenPipe (for example),
 *   the dynamic linker resolves the GOT and overwrites it with the
 *   real libusbd.sprx function address. If that happens, *got_slot
 *   points into system PRX memory, not the game's PLT stub.
 *
 *   FUTURE IMPROVEMENT (Option B):
 *   Instead of reading *got_slot, scan the game's .text segment for
 *   the actual 16-byte PLT stub pattern:
 *     lis   r11, offset_hi   0x3D60xxxx
 *     lwz   r12, offset_lo(r11)  0x818Bxxxx
 *     mtctr r12              0x7D8903A6
 *     bctr                   0x4E800420
 *   The address of that stub is the true TARGET — it never changes,
 *   regardless of GOT resolution. This is LEFT FOR FUTURE IMPLEMENTATION.
 *   Option A is used now because we inject before the game calls
 *   cellUsbd. If it fails, recompile with Option B.
 *
 * Returns 0 on success, -1 if stub not found.
 * ================================================================ */
#define NID_SCAN_START  0x00100000u
#define NID_SCAN_SIZE   0x00A00000u  /* 10MB — covers game .text/.rodata */

/* ================================================================
 * PING-AND-SCAN GOT FINDER (Expert-Recommended, 2026-07-24)
 *
 * The game's NID table is stripped at runtime. We force our SPRX's
 * imports to resolve, extract the real OS addresses from our GOT,
 * and scan the game's data segment for those exact 32-bit pointers.
 * ================================================================ */

/**
 * get_real_os_address — Parse our SPRX's GCC PRX import stub to
 * read the resolved function address from our own GOT.
 *
 * GCC -mprx stubs: lis rX, got_hi / lwz rX, got_lo(rX)
 */
static uint32_t get_real_os_address(void *func_ptr)
{
    uint32_t *stub = (uint32_t *)func_ptr;
    uint32_t w0 = stub[0];
    uint32_t w1 = stub[1];
    uint32_t rt, hi, lo;

    DEBUG_PRINT("[USB] ping: stub=%p [0]=0x%08X [1]=0x%08X\n",
                func_ptr, (unsigned)w0, (unsigned)w1);

    /* Case 1: func_ptr points directly to stub code (lis/addis pattern).
     * Check: first word has addis opcode (0x3C). */
    if ((w0 & 0xFC000000) == 0x3C000000) {
        rt = (w0 >> 21) & 0x1F;
        hi = w0 & 0xFFFF;
        /* lwz rX, lo(rX) — matching register */
        if ((w1 & 0xFC000000) != 0x80000000) { DEBUG_PRINT("[USB] ping: not lwz\n"); return 0; }
        if (((w1 >> 16) & 0x1F) != rt || ((w1 >> 21) & 0x1F) != rt) {
            DEBUG_PRINT("[USB] ping: reg mismatch\n"); return 0;
        }
        lo = w1 & 0xFFFF;
        {
            int16_t slo = (int16_t)lo;
            uint32_t got_addr = (hi << 16) + (uint32_t)(int32_t)slo;
            volatile uint32_t *got_slot = (volatile uint32_t *)(uintptr_t)got_addr;
            uint32_t resolved = *got_slot;
            DEBUG_PRINT("[USB] ping: direct stub GOT=0x%08X -> 0x%08X\n",
                        (unsigned)got_addr, (unsigned)resolved);
            if (resolved >= 0x30000000 && resolved <= 0x4FFFFFFF) return resolved;
        }
        return 0;
    }

    /* Case 2: func_ptr points to an OPD (Official Procedure Descriptor).
     * OPD layout: { code_addr, toc_addr, env_ptr }.
     * The code_addr points to the actual import stub code.
     * Follow the OPD to read the stub, then parse it. */
    if (w0 >= 0x00010000 && w0 <= 0x4FFFFFFF) {
        uint32_t code_addr = w0;
        uint32_t *real_stub = (uint32_t *)(uintptr_t)code_addr;
        uint32_t sw0 = real_stub[0];
        uint32_t sw1 = real_stub[1];

        DEBUG_PRINT("[USB] ping: OPD follow: code=0x%08X stub[0]=0x%08X stub[1]=0x%08X\n",
                    (unsigned)code_addr, (unsigned)sw0, (unsigned)sw1);

        /* Now parse the stub at code_addr for lis/addis + lwz */
        if ((sw0 & 0xFC000000) == 0x3C000000) {
            rt = (sw0 >> 21) & 0x1F;
            hi = sw0 & 0xFFFF;
            if ((sw1 & 0xFC000000) != 0x80000000) {
                DEBUG_PRINT("[USB] ping: OPD stub w1 not lwz\n");
                return 0;
            }
            if (((sw1 >> 16) & 0x1F) != rt || ((sw1 >> 21) & 0x1F) != rt) {
                DEBUG_PRINT("[USB] ping: OPD stub reg mismatch\n");
                return 0;
            }
            lo = sw1 & 0xFFFF;
            {
                int16_t slo = (int16_t)lo;
                uint32_t got_addr = (hi << 16) + (uint32_t)(int32_t)slo;
                volatile uint32_t *got_slot = (volatile uint32_t *)(uintptr_t)got_addr;
                uint32_t resolved = *got_slot;
                DEBUG_PRINT("[USB] ping: OPD stub GOT=0x%08X -> 0x%08X\n",
                            (unsigned)got_addr, (unsigned)resolved);
                if (resolved >= 0x30000000 && resolved <= 0x4FFFFFFF) return resolved;
            }
        } else {
            DEBUG_PRINT("[USB] ping: OPD stub not addis/lis (0x%08X)\n", (unsigned)sw0);
        }
    }

    return 0;
}

/**
 * find_game_got_slot — Scan game's data segment for a known 32-bit
 * value (the real OS address). Returns pointer to the GOT slot.
 */
static uint32_t *find_game_got_slot(uint32_t real_addr)
{
    uint32_t scan_start = 0x00880000;
    uint32_t scan_end   = 0x02000000;
    uint32_t *words = (uint32_t *)(uintptr_t)scan_start;
    uint32_t nwords = (scan_end - scan_start) / 4;
    uint32_t i;

    DEBUG_PRINT("[USB] scan: searching 0x%08X-0x%08X (%u words) for 0x%08X\n",
                (unsigned)scan_start, (unsigned)scan_end, (unsigned)nwords, (unsigned)real_addr);

    for (i = 0; i < nwords; i++) {
        if (words[i] == real_addr) {
            DEBUG_PRINT("[USB] scan: MATCH at word %u -> GOT slot 0x%08X\n",
                        (unsigned)i, (unsigned)(uintptr_t)&words[i]);
            return &words[i];
        }
    }
    DEBUG_PRINT("[USB] scan: no match for 0x%08X in range\n", (unsigned)real_addr);
    return NULL;
}

/* ---- Original NID scanner (get_game_plt_stub) follows ---- */

static int get_game_plt_stub(uint32_t target_nid, uint32_t *out_plt_addr)
{
    volatile uint32_t *words = (volatile uint32_t*)(uintptr_t)NID_SCAN_START;
    uint32_t nwords = NID_SCAN_SIZE / 4;
    uint32_t i;

    if (out_plt_addr == NULL) return -1;

    /* Scan for 12-byte triplet pattern { NID, reserved, GOT_ptr }
     * at 3-word intervals. This is the PS3's import stub table format
     * in the game's .rodata section. */
    for (i = 0; i <= nwords - 3; i += 3) {
        uint32_t nid      = words[i + 0];
        uint32_t reserved = words[i + 1];
        uint32_t got_ptr  = words[i + 2];

        if (nid != target_nid) continue;

        /* Sanity check: reserved should be 0 or a small value.
         * GOT_ptr must point to valid (non-zero) memory in game range. */
        if (reserved != 0 && reserved > 0x1000) continue;
        if (got_ptr == 0 || got_ptr < 0x00010000 || got_ptr > 0x4FFFFFFF) continue;

        /* Read the GOT slot value — this is the PLT stub address
         * (if unresolved) or the real function address (if resolved).
         *
         * ⚠ CAVEAT: See note above about Option A vs Option B.
         * If the function has been resolved, this read returns the
         * real libusbd.sprx address instead of the game's PLT stub.
         * We assume injection happens early enough that GOT is
         * unresolved. */
        {
            volatile uint32_t *got_slot = (volatile uint32_t*)(uintptr_t)got_ptr;
            uint32_t plt_stub_addr = *got_slot;

            /* PLT stubs live in game .text (typically < 0x01000000
             * for small games, or higher for larger ones). If the
             * value is in the PRX range (0x3xxxxxxx+), the GOT has
             * been resolved and we cannot safely overwrite it. */
            if (plt_stub_addr == 0 || plt_stub_addr > 0x4FFFFFFF) continue;

            if (plt_stub_addr >= 0x30000000) {
                /* GOT is already resolved — writing a preamble here
                 * would corrupt libusbd.sprx code. Hard-fail. */
                DEBUG_ERROR("[USB] NID scan: NID=0x%08X GOT_ptr=0x%08X -> *GOT=0x%08X (RESOLVED)\n",
                            (unsigned)nid, (unsigned)got_ptr, (unsigned)plt_stub_addr);
                DEBUG_ERROR("[USB]   GOT resolved! Injection too late. Implement Option B PLT-pattern scanner.\n");
                *out_plt_addr = 0;  /* Signal injector to skip this hook */
                return -1;
            }

            *out_plt_addr = plt_stub_addr;

            DEBUG_PRINT("[USB] NID scan: NID=0x%08X GOT_ptr=0x%08X -> *GOT=0x%08X (ok, unresolved)\n",
                        (unsigned)nid, (unsigned)got_ptr, (unsigned)plt_stub_addr);
            DEBUG_PRINT("[USB]   PLT stub at 0x%08X will be overwritten with preamble\n",
                        (unsigned)plt_stub_addr);
            return 0;
        }
    }

    DEBUG_ERROR("[USB] NID scan: NID=0x%08X not found in range 0x%08X-0x%08X\n",
                (unsigned)target_nid, (unsigned)NID_SCAN_START,
                (unsigned)(NID_SCAN_START + NID_SCAN_SIZE));
    return -1;
}

/* ================================================================
 * usb_hook_init - REFACTORED 2026-07-22
 *
 * Init sequence:
 *   1. OPD validation of cellUsbd imports
 *   2. Allocate R-W-X page + install dynamic trampolines
 *   3. Scan game for PLT stub addresses (TARGET_*)
 *   4. Write IPC file for Node.js orchestrator
 * ================================================================ */

int usb_hook_init(void)
{
    int ret;
    uint32_t target_init = 0, target_openpipe = 0, target_transfer = 0, target_closepipe = 0;

    if (g_usb_hooks.initialized) return 0;
    memset(&g_usb_hooks, 0, sizeof(g_usb_hooks));
    g_usb_hooks.next_pipe_id = 0x1000;

    /* Step 1: Validate cellUsbd imports via OPD extraction (SOFT fail).
     *
     * We extract the resolved code addresses from our own SPRX's OPD
     * import entries. The SPRX is linked with -lusbd_stub, so CellOS
     * resolves cellUsbd imports when our module is loaded. Casting
     * the imported function symbol to a ppc_opd_t* gives us the real
     * code address and TOC pointer.
     *
     * IMPORTANT: On some CellOS PRX link setups, the extern imported
     * symbols (cellUsbdInit, etc.) may point to import stub code in
     * the SPRX's .text section rather than to OPD entries. In that
     * case, casting to ppc_opd_t* and dereferencing code_addr reads
     * the first instruction opcode as an address — which fails the
     * range check and returns -1. This is NOT fatal because:
     *
     *   1. create_hook_trampoline() uses the OPDs of OUR OWN C hook
     *      functions (my_cellUsbdInit, my_cellUsbdOpenPipe, etc.),
     *      NOT the cellUsbd import OPDs. Our functions always have
     *      valid OPDs in the SPRX's data section.
     *
     *   2. Passthrough calls (non-ToyPad USB traffic) just call
     *      cellUsbdOpenPipe() directly from C. The compiler uses our
     *      own GOT/TOC via the import stub — it doesn't read the OPD
     *      manually. The import stub works fine even when the OPD
     *      trick fails.
     *
     * So: if OPD extraction fails, log a warning and continue. The
     * trampolines and passthrough calls will still work correctly. */
    ret = find_cellusbd_functions_via_opd();
    if (ret != 0) {
        DEBUG_ERROR("[USB] OPD extraction soft-fail - continuing (trampolines use own OPDs)\n");
        /* NOT returning -1 here! The hook mechanism works fine without
         * cellUsbd import OPDs. Only the validation is skipped. */
    }

    /* Step 2: Allocate executable trampoline page and install hooks */
    if (install_hooks() != 0) {
        DEBUG_ERROR("[USB] Hook installation failed\n");
        return -1;
    }

    /* Step 3: Scan game memory for PLT stub addresses (TARGET_*).
     *
     * We scan the game's import stub table (.rodata) for NID triplets
     * matching cellUsbdInit, cellUsbdOpenPipe, cellUsbdInterruptTransfer,
     * and cellUsbdClosePipe. For each NID found, we read the associated
     * GOT slot to obtain the PLT stub address.
     *
     * This is a best-effort scan. If a particular NID is not found
     * (e.g., the game doesn't import that function, or the scanner
     * range doesn't cover the game's .rodata), the target address
     * is written as 0x00000000. The Node.js injector will skip that
     * hook if the target is 0.
     *
     * ⚠ OPTION A CAVEAT: The *got_slot value is only the PLT stub
     * if the GOT is unresolved. If the game has already called the
     * function, *got_slot points to the real libusbd.sprx function.
     * Future Option B (PLT-pattern scanner) should replace this.
     * See get_game_plt_stub() documentation above. */

    /* Step 3: PING-AND-SCAN GOT OVERWRITE (Expert-Recommended 2026-07-24).
     * NID tables stripped at runtime. Use our SPRX's resolved imports.
     *
     * 3a. Ping cellUsbd functions → force SPRX GOT resolution.
     * 3b. Parse import stubs → extract real OS addresses (0x30XXXXXX).
     * 3c. Scan game data segment for those addresses → GOT slots.
     * 3d. Direct overwrite each GOT slot with trampoline + cache sync. */
    DEBUG_PRINT("[USB] === PING-AND-SCAN GOT OVERWRITE ===\n");
    INIT_PROGRESS(61);
    {
        uint32_t tramp_init     = g_usb_hooks.trampoline_base + g_usb_hooks.tramp_init_offset;
        uint32_t tramp_openpipe = g_usb_hooks.trampoline_base + g_usb_hooks.tramp_open_pipe_offset;
        uint32_t tramp_transfer = g_usb_hooks.trampoline_base + g_usb_hooks.tramp_transfer_offset;
        uint32_t tramp_closepipe = g_usb_hooks.trampoline_base + g_usb_hooks.tramp_close_pipe_offset;
        uint32_t real_init = 0, real_open = 0, real_transfer = 0, real_close = 0;
        uint32_t *got_init = NULL, *got_open = NULL, *got_transfer = NULL, *got_close = NULL;
        int hooks_installed = 0;

        /* 3a: Ping to force lazy-binding resolution */
        DEBUG_PRINT("[USB] ping: forcing SPRX import resolution...\n");
        cellUsbdInit();
        cellUsbdClosePipe(0xFFFFFFFF);

        /* 3b: Extract real OS addresses from our SPRX's resolved GOT */
        INIT_PROGRESS(62);
        real_init     = get_real_os_address((void*)cellUsbdInit);
        real_open     = get_real_os_address((void*)cellUsbdOpenPipe);
        real_transfer = get_real_os_address((void*)cellUsbdInterruptTransfer);
        real_close    = get_real_os_address((void*)cellUsbdClosePipe);

        DEBUG_PRINT("[USB] ping result: init=0x%08X open=0x%08X xfer=0x%08X close=0x%08X\n",
                    (unsigned)real_init, (unsigned)real_open,
                    (unsigned)real_transfer, (unsigned)real_close);

        if (!real_init || !real_open || !real_transfer || !real_close) {
            DEBUG_ERROR("[USB] FAIL: Could not resolve %d/4 SPRX imports\n",
                        (real_init?1:0)+(real_open?1:0)+(real_transfer?1:0)+(real_close?1:0));
        } else {
            /* 3c: Scan game memory for matching GOT slots */
            INIT_PROGRESS(63);
            DEBUG_PRINT("[USB] scan: searching game data for resolved addresses...\n");
            got_init     = find_game_got_slot(real_init);
            got_open     = find_game_got_slot(real_open);
            got_transfer = find_game_got_slot(real_transfer);
            got_close    = find_game_got_slot(real_close);

            DEBUG_PRINT("[USB] scan result: init=%p open=%p xfer=%p close=%p\n",
                        (void*)got_init, (void*)got_open,
                        (void*)got_transfer, (void*)got_close);

            /* 3d: Overwrite each GOT slot + cache sync */
            INIT_PROGRESS(64);
            if (got_init) {
                target_init = (uint32_t)(uintptr_t)got_init;
                DEBUG_PRINT("[USB] GOT INIT: *0x%08X = 0x%08X (was 0x%08X)\n",
                            (unsigned)target_init, (unsigned)tramp_init, (unsigned)real_init);
                *got_init = tramp_init;
                __asm__ __volatile__ ("dcbst 0, %0\n\tsync\n\ticbi 0, %0\n\tisync" :: "r"(got_init) : "memory");
                hooks_installed++;
            } else { DEBUG_ERROR("[USB] GOT INIT: not found\n"); }

            if (got_open) {
                target_openpipe = (uint32_t)(uintptr_t)got_open;
                DEBUG_PRINT("[USB] GOT OPEN: *0x%08X = 0x%08X\n",
                            (unsigned)target_openpipe, (unsigned)tramp_openpipe);
                *got_open = tramp_openpipe;
                __asm__ __volatile__ ("dcbst 0, %0\n\tsync\n\ticbi 0, %0\n\tisync" :: "r"(got_open) : "memory");
                hooks_installed++;
            } else { DEBUG_ERROR("[USB] GOT OPEN: not found\n"); }

            if (got_transfer) {
                target_transfer = (uint32_t)(uintptr_t)got_transfer;
                DEBUG_PRINT("[USB] GOT XFER: *0x%08X = 0x%08X\n",
                            (unsigned)target_transfer, (unsigned)tramp_transfer);
                *got_transfer = tramp_transfer;
                __asm__ __volatile__ ("dcbst 0, %0\n\tsync\n\ticbi 0, %0\n\tisync" :: "r"(got_transfer) : "memory");
                hooks_installed++;
            } else { DEBUG_ERROR("[USB] GOT XFER: not found\n"); }

            if (got_close) {
                target_closepipe = (uint32_t)(uintptr_t)got_close;
                DEBUG_PRINT("[USB] GOT CLOSE: *0x%08X = 0x%08X\n",
                            (unsigned)target_closepipe, (unsigned)tramp_closepipe);
                *got_close = tramp_closepipe;
                __asm__ __volatile__ ("dcbst 0, %0\n\tsync\n\ticbi 0, %0\n\tisync" :: "r"(got_close) : "memory");
                hooks_installed++;
            } else { DEBUG_ERROR("[USB] GOT CLOSE: not found\n"); }

            DEBUG_PRINT("[USB] %d/4 GOT slots overwritten via ping-and-scan\n", hooks_installed);
        }
    }
    INIT_PROGRESS(67);

    /* Step 4: Write IPC file for Node.js orchestrator.
     *
     * The IPC file contains:
     *   - TRAMP_* addresses (trampoline locations in our R-W-X page)
     *   - TARGET_* addresses (PLT stub locations in game .text)
     *
     * Node.js reads these, then writes 4-instruction preambles
     * (lis/ori/mtctr/bctr) into the game's .text segment via
     * PS3MAPI /write_process. Each preamble replaces a PLT stub
     * and branches to the corresponding trampoline, which handles
     * TOC save/restore and calls our C hook. */
    write_ipc_file(target_init, target_openpipe, target_transfer, target_closepipe);

    if (target_init != 0) {
        DEBUG_PRINT("[USB] TARGET_INIT=0x%08X -> TRAMP_INIT=0x%08X\n",
                    (unsigned)target_init,
                    (unsigned)(g_usb_hooks.trampoline_base + g_usb_hooks.tramp_init_offset));
    } else {
        DEBUG_PRINT("[USB] TARGET_INIT=0x00000000 (NID not found — injector will skip)\n");
    }
    if (target_openpipe != 0) {
        DEBUG_PRINT("[USB] TARGET_OPENPIPE=0x%08X -> TRAMP_OPENPIPE=0x%08X\n",
                    (unsigned)target_openpipe,
                    (unsigned)(g_usb_hooks.trampoline_base + g_usb_hooks.tramp_open_pipe_offset));
    }
    if (target_transfer != 0) {
        DEBUG_PRINT("[USB] TARGET_TRANSFER=0x%08X -> TRAMP_TRANSFER=0x%08X\n",
                    (unsigned)target_transfer,
                    (unsigned)(g_usb_hooks.trampoline_base + g_usb_hooks.tramp_transfer_offset));
    }
    if (target_closepipe != 0) {
        DEBUG_PRINT("[USB] TARGET_CLOSEPIPE=0x%08X -> TRAMP_CLOSEPIPE=0x%08X\n",
                    (unsigned)target_closepipe,
                    (unsigned)(g_usb_hooks.trampoline_base + g_usb_hooks.tramp_close_pipe_offset));
    }

    g_usb_hooks.initialized = 1;
    DEBUG_PRINT("[USB] All 4 hooks installed, awaiting Node.js preamble\n");
    return 0;
}

/* ================================================================
 * usb_hook_shutdown - REFACTORED
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
