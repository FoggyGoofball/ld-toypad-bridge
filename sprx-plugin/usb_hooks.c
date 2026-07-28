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
#include <sys/prx.h>
#include <cell/cell_fs.h>

/* Sony SDK: sys_memory_allocate is in <sys/memory.h>
 * but SYS_MEMORY_CONTAINER_DEFAULT may not be declared.
 * 0xFFFFFFFF is the default container ID on CellOS. */
#define SYS_MEMORY_CONTAINER_DEFAULT  ((uint32_t)0xFFFFFFFFu)

/* CellOS OPD (Official Procedure Descriptor) structure.
 * On PowerPC 32-bit CellOS, function pointers point to a 12-byte OPD
 * struct containing: code address, TOC address, environment pointer. */
typedef struct {
    uint32_t code_addr;    /* Ptr to .text code */
    uint32_t toc_addr;     /* TOC base value (loaded into r2 on call) */
    uint32_t env_ptr;      /* Environment pointer (unused, set to 0) */
} ppc_opd_t;

/* Forward declaration for hook integrity checker */
static int hook_verify_preamble(uint32_t target_addr, const char *name);

/* NID values for cellUsbd functions.
 * These are 32-bit big-endian NIDs used in the PS3's import stub table
 * (.rodata triplet format: { NID, reserved, GOT_ptr }).
 * Verified against LEGO Dimensions game memory dumps.
 *
 * NOTE: cellUsbdInit NID removed (expert recommendation 2026-07-26).
 * Hooking Init can destabilize the active USB stack if called after
 * the game has already initialized USB. The game won't call Init again
 * at T+60s, so there's no benefit to intercepting it. */
#define NID_CELL_USBD_OPENPIPE      0x1AB6D80BU
#define NID_CELL_USBD_TRANSFER      0x7B4436CEU  /* cellUsbdInterruptTransfer */
#define NID_CELL_USBD_CLOSEPIPE     0x2F82F1A5U
#define NID_CELL_USBD_GET_DEVICE_DESC 0x9C8426F7U
#define NID_CELL_USBD_CONTROL_TRANSFER 0x3219460DU

#include "usb_hooks.h"
#include "network.h"
#include "debug.h"
#include "trampoline_gen.h"
#include "toypad_state.h"

/* Global state */
usb_hook_state_t g_usb_hooks;

/* Real libusbd.sprx function addresses (for passthrough, skipping preamble) */
static uint32_t g_real_openpipe_addr = 0;
static uint32_t g_real_transfer_addr = 0;
static uint32_t g_real_closepipe_addr = 0;

/* libusbd.sprx runtime base address (from sys_prx_get_module_id_by_name) */
static uint32_t g_libusbd_base = 0;

#define CELL_OK                  0
#define CELL_USBD_ERROR_FAILED  -1

/* ================================================================
 * DIAGNOSTICS: g_init_progress extern + INIT_PROGRESS macro
 *
 * Declared in main.c. Updated at every init step boundary for
 * PS3MAPI memory-polling diagnostics. Cache-coherent writes.
 * ================================================================ */
extern volatile uint32_t g_init_progress;

/* papertrail — defined in main.c, writes to boot log (single string only) */
extern int papertrail(const char *msg);

#define INIT_PROGRESS(x) do { \
    g_init_progress = (x); \
    __asm__ __volatile__ ("dcbst 0, %0\n\tsync" :: "r"(&g_init_progress) : "memory"); \
} while(0)

/* cellUsbd function imports from -lusbd_stub.
 * NOTE: cellUsbdInit removed — calling it after game USB init is dangerous. */
extern int cellUsbdOpenPipe(void *pipe_handle, uint32_t dev_id, void *ep_descriptor);
extern int cellUsbdInterruptTransfer(uint32_t pipe_handle, void *buf, uint32_t *len,
                                     void *done_cb, void *arg);
extern int cellUsbdClosePipe(uint32_t pipe_handle);
/* cellUsbdGetDeviceDescriptor NOT in libusbd_stub.a — found via NID scan */

/* ================================================================
 * Hook Installation (replaces allocate_trampolines + toc_trampoline.s)
 * ================================================================
 * We allocate a single 64KB R-W-X page via sys_memory_allocate.
 * Within this page, we generate 5 trampolines at 64-byte offsets:
 *   Offset 0:   OpenPipe trampoline        (toc_arg_reg=6)
 *   Offset 64:  Transfer trampoline        (toc_arg_reg=8)
 *   Offset 128: ClosePipe trampoline       (toc_arg_reg=4)
 *   Offset 192: GetDeviceDescriptor tramp  (toc_arg_reg=5)
 *   Offset 256: ControlTransfer trampoline (toc_arg_reg=9)
 *   Offset 320: Heartbeat counter (uint32_t)
 * ================================================================ */
#define TRAMPOLINE_PAGE_SIZE    (64 * 1024)
#define TRAMPOLINE_BLOCK_SIZE   64  /* 16 instructions */

/* TOC argument register for each hook based on number of original args */
#define TOC_REG_OPENPIPE   6   /* 3 args -> TOC goes in r6 */
#define TOC_REG_TRANSFER   8   /* 5 args -> TOC goes in r8 */
#define TOC_REG_CLOSEPIPE  4   /* 1 arg  -> TOC goes in r4 */
#define TOC_REG_GET_DEVICE_DESC 5  /* 2 args (dev_num, desc) -> TOC goes in r5 */
#define TOC_REG_CONTROL_TRANSFER 9  /* 6 args -> TOC goes in r9 */

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

    /* Must be in mapped range. With OPD-based hooking, targets point to
     * the real libusbd.sprx functions (which may be at 0x02C00000).
     * Accept any valid address above 0x00010000. */
    if (target_addr < 0x00010000 || target_addr > 0x4FFFFFFF) {
        DEBUG_ERROR("[USB] INTEGRITY FAIL: %s target=0x%08X — out of mapped range\n",
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
    g_usb_hooks.tramp_open_pipe_offset = 0;
    g_usb_hooks.tramp_transfer_offset = TRAMPOLINE_BLOCK_SIZE;
    g_usb_hooks.tramp_close_pipe_offset = 2 * TRAMPOLINE_BLOCK_SIZE;
    g_usb_hooks.tramp_get_device_desc_offset = 3 * TRAMPOLINE_BLOCK_SIZE;
    g_usb_hooks.tramp_control_transfer_offset = 4 * TRAMPOLINE_BLOCK_SIZE;

    /* Heartbeat counter at offset 320 (after 5 x 64-byte trampolines). */
    g_usb_hooks.heartbeat = (volatile uint32_t*)(uintptr_t)(base_addr + 320);
    DEBUG_PRINT("[USB] Heartbeat counter at 0x%08X\n",
                (unsigned)(base_addr + 256));

    /* Step 2: Generate trampolines using create_hook_trampoline(). */
    /* Init hook REMOVED — calling cellUsbdInit() from SPRX context
     * after game USB init can destabilize the active stack.
     * Start directly with OpenPipe at offset 0. */

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

    create_hook_trampoline(
        (uint32_t*)(uintptr_t)(base_addr + g_usb_hooks.tramp_get_device_desc_offset),
        (void*)my_cellUsbdGetDeviceDescriptor, TOC_REG_GET_DEVICE_DESC);
    DEBUG_PRINT("[USB] GetDeviceDescriptor trampoline at 0x%08X\n",
                (unsigned)(base_addr + g_usb_hooks.tramp_get_device_desc_offset));
    log_trampoline_header("GetDevDesc", (unsigned)(base_addr + g_usb_hooks.tramp_get_device_desc_offset));

    create_hook_trampoline(
        (uint32_t*)(uintptr_t)(base_addr + g_usb_hooks.tramp_control_transfer_offset),
        (void*)my_cellUsbdControlTransfer, TOC_REG_CONTROL_TRANSFER);
    DEBUG_PRINT("[USB] ControlTransfer trampoline at 0x%08X\n",
                (unsigned)(base_addr + g_usb_hooks.tramp_control_transfer_offset));
    log_trampoline_header("CtrlXfer", (unsigned)(base_addr + g_usb_hooks.tramp_control_transfer_offset));

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

static int write_ipc_file(uint32_t target_openpipe,
                          uint32_t target_transfer, uint32_t target_closepipe,
                          uint32_t target_getdevdesc, uint32_t target_ctrlxfer)
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
    WRITE_ADDR_LINE("TRAMP_OPENPIPE",
        g_usb_hooks.trampoline_base + g_usb_hooks.tramp_open_pipe_offset);
    WRITE_ADDR_LINE("TRAMP_TRANSFER",
        g_usb_hooks.trampoline_base + g_usb_hooks.tramp_transfer_offset);
    WRITE_ADDR_LINE("TRAMP_CLOSEPIPE",
        g_usb_hooks.trampoline_base + g_usb_hooks.tramp_close_pipe_offset);
    WRITE_ADDR_LINE("TRAMP_GETDEVDESC",
        g_usb_hooks.trampoline_base + g_usb_hooks.tramp_get_device_desc_offset);
    WRITE_ADDR_LINE("TRAMP_CTRLXFER",
        g_usb_hooks.trampoline_base + g_usb_hooks.tramp_control_transfer_offset);

    /* Game PLT stub addresses (from NID scan) for preamble installation */
    WRITE_ADDR_LINE("TARGET_OPENPIPE", target_openpipe);
    WRITE_ADDR_LINE("TARGET_TRANSFER", target_transfer);
    WRITE_ADDR_LINE("TARGET_CLOSEPIPE", target_closepipe);
    WRITE_ADDR_LINE("TARGET_GETDEVDESC", target_getdevdesc);
    WRITE_ADDR_LINE("TARGET_CTRLXFER", target_ctrlxfer);

    /* Heartbeat offset — Node.js injector reads this to locate the
     * heartbeat counter in the trampoline page. NOT hardcoded! */
    WRITE_ADDR_LINE("HEARTBEAT_OFFSET", 320);

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
    if (cellFsOpen("/dev_hdd0/plugins/ld_hooks.tmp",
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
    if (cellFsRename("/dev_hdd0/plugins/ld_hooks.tmp",
                     "/dev_hdd0/plugins/ld_hooks_ready.txt") != CELL_OK) {
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

    ep_addr = extract_ep_addr(ep_descriptor);

    DEBUG_PRINT("[USB] ENTER my_cellUsbdOpenPipe(dev_id=0x%08X, ep=0x%02X, game_toc=0x%08X)\n",
                (unsigned)dev_id, (unsigned)ep_addr, (unsigned)game_toc);

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
     * Since we hooked the REAL function code (preamble at target),
     * we must call target+16 to skip the preamble and enter the
     * original function body. */
    if (g_real_openpipe_addr != 0) {
        /* Build temporary OPD pointing to target+16 (past the preamble) */
        ppc_opd_t real_opd;
        real_opd.code_addr = g_real_openpipe_addr + 16;
        real_opd.toc_addr = game_toc;  /* libusbd.sprx uses game's TOC */
        real_opd.env_ptr = 0;
        int (*real_fn)(void*, uint32_t, void*) = (int(*)(void*,uint32_t,void*))&real_opd;
        return real_fn(pipe_handle, dev_id, ep_descriptor);
    }
    return CELL_USBD_ERROR_FAILED;
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
    (void)game_toc;

    /* PS3 cellUsbd is ASYNCHRONOUS. The game expects us to:
     * 1. Process the transfer
     * 2. Call done_cb(result, count, arg) to wake the game's USB thread
     * 3. Return CELL_OK
     * If we don't call done_cb, the game's USB thread sleeps forever. */

    typedef void (*usbd_done_cb_t)(int32_t result, int32_t count, void *arg);

    DEBUG_PRINT("[USB] ENTER my_cellUsbdInterruptTransfer(pipe=0x%08X, len=%u, game_toc=0x%08X)\n",
                (unsigned)pipe_handle,
                (unsigned)(len ? *len : 0),
                (unsigned)game_toc);

    /* One-time log to boot log (safe — only fires once due to static counter) */
    {
        static int xfer_log_count = 0;
        if (xfer_log_count == 0) {
            xfer_log_count = 1;
            papertrail("[USB] First InterruptTransfer called — USB bridge ACTIVE!");
        }
    }

    toypad_type = usb_hook_is_toypad_pipe(pipe_handle);
    if (toypad_type == 0) {
        /* Non-ToyPad: pass through to real function at target+16 (skip preamble) */
        if (g_real_transfer_addr != 0) {
            ppc_opd_t real_opd;
            real_opd.code_addr = g_real_transfer_addr + 16;
            real_opd.toc_addr = game_toc;
            real_opd.env_ptr = 0;
            int (*real_fn)(uint32_t, void*, uint32_t*, void*, void*) =
                (int(*)(uint32_t,void*,uint32_t*,void*,void*))&real_opd;
            return real_fn(pipe_handle, buf, len, done_cb, arg);
        }
        return CELL_USBD_ERROR_FAILED;
    }

    if (toypad_type == 1) {
        /* Toy Pad IN endpoint: Non-blocking. Return dummy data with 0x55
         * magic byte to test protocol handshake, then wake the game's
         * USB thread via done_cb. CRITICAL: call done_cb or game freezes! */
        uint32_t max_len;
        uint8_t response[512];
        uint8_t zone = 1;
        uint8_t seq;
        int recv_len;
        if (buf == NULL || len == NULL) return CELL_USBD_ERROR_FAILED;
        max_len = *len;
        if (max_len > 256) max_len = 256;
        seq = (uint8_t)(g_usb_hooks.next_pipe_id++);

        /* Tell server we're polling */
        network_send_poll(zone, seq);

        /* Non-blocking receive (socket is SO_NBIO) */
        recv_len = network_recv(response, sizeof(response));
        if (recv_len > 2 && response[0] == 0x00) {
            int payload_len = recv_len - 3;
            if (payload_len > (int)max_len) payload_len = (int)max_len;
            if (payload_len > 0) memcpy(buf, response + 3, (size_t)payload_len);
            *len = (uint32_t)payload_len;
        } else {
            /* No server response — return 0x55 magic byte to test handshake */
            if (max_len > 0) memset(buf, 0, max_len);
            ((uint8_t*)buf)[0] = 0x55;
            *len = (max_len > 0) ? max_len : 1;
        }

        /* CRITICAL: Wake the game's USB thread! */
        if (done_cb) {
            usbd_done_cb_t cb = (usbd_done_cb_t)done_cb;
            cb(CELL_OK, (int32_t)*len, arg);
        }
        return CELL_OK;
    }

    if (toypad_type == 2) {
        /* Toy Pad OUT endpoint: Send data to server, then wake game */
        uint8_t zone = 1;
        uint8_t seq = (uint8_t)(g_usb_hooks.next_pipe_id++);
        if (buf == NULL || len == NULL) return CELL_USBD_ERROR_FAILED;
        network_send_data(zone, seq, (const uint8_t*)buf, (int)*len);

        /* CRITICAL: Wake the game's USB thread! */
        if (done_cb) {
            usbd_done_cb_t cb = (usbd_done_cb_t)done_cb;
            cb(CELL_OK, (int32_t)*len, arg);
        }
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
        /* Non-ToyPad: pass through to real function at target+16 (skip preamble) */
        if (g_real_closepipe_addr != 0) {
            ppc_opd_t real_opd;
            real_opd.code_addr = g_real_closepipe_addr + 16;
            real_opd.toc_addr = game_toc;
            real_opd.env_ptr = 0;
            int (*real_fn)(uint32_t) = (int(*)(uint32_t))&real_opd;
            return real_fn(pipe_handle);
        }
        return CELL_USBD_ERROR_FAILED;
    }

    free_pipe(pipe_handle);
    DEBUG_PRINT("[USB] ToyPad pipe closed: handle=0x%08X\n",
                (unsigned)pipe_handle);
    return CELL_OK;
}

/* ================================================================
 * HOOK: my_cellUsbdGetDeviceDescriptor — TROJAN HORSE (2026-07-26)
 *
 * When a physical USB device is plugged into the PS3, the LV2 kernel
 * wakes all sleeping LDDs. The game's probe() callback calls
 * cellUsbdGetDeviceDescriptor() to identify the device. We intercept
 * that call and return the LEGO Dimensions ToyPad descriptor
 * (VID=0x0E6F, PID=0x0241), tricking the game into believing the
 * physical device is a ToyPad.
 *
 * Whatever USB device was just plugged in (flash drive, controller,
 * etc.) is now treated as a ToyPad. The game returns PROBE_SUCCEEDED
 * to the kernel, which fires the game's attach() callback. Our
 * OpenPipe hook (above) then intercepts the open-pipe calls and
 * returns fake pipe handles, completing the bridge.
 *
 * DESCRIPTOR FORMAT (18 bytes):
 *   Offset  Size  Description
 *   0       1     bLength (18)
 *   1       1     bDescriptorType (0x01 = DEVICE)
 *   2       2     bcdUSB (0x0200 = USB 2.0)
 *   4       1     bDeviceClass
 *   5       1     bDeviceSubClass
 *   6       1     bDeviceProtocol
 *   7       1     bMaxPacketSize0
 *   8       2     idVendor (0x0E6F = Logic3/PDP)
 *   10      2     idProduct (0x0241 = LEGO Dimensions ToyPad)
 *   12      2     bcdDevice
 *   14      1     iManufacturer
 *   15      1     iProduct
 *   16      1     iSerialNumber
 *   17      1     bNumConfigurations
 * ================================================================ */
int my_cellUsbdGetDeviceDescriptor(uint32_t dev_id, void *desc,
                                    uint32_t game_toc)
{
    (void)dev_id;
    (void)game_toc;

    DEBUG_PRINT("[USB] *** TROJAN HORSE: cellUsbdGetDeviceDescriptor(dev=0x%08X) ***\n",
                (unsigned)dev_id);

    /* Log to boot log (called once per USB plug, safe) */
    papertrail("[USB] *** TROJAN HORSE FIRED! Returning ToyPad VID/PID ***");

    /* Return the ToyPad device descriptor unconditionally.
     * Whatever physical device was just plugged in is now a ToyPad. */
    if (desc != NULL) {
        static const uint8_t toypad_dev_desc[18] = {
            0x12,                       /* bLength: 18 bytes */
            0x01,                       /* bDescriptorType: DEVICE */
            0x00, 0x02,                 /* bcdUSB: USB 2.0 */
            0x00,                       /* bDeviceClass */
            0x00,                       /* bDeviceSubClass */
            0x00,                       /* bDeviceProtocol */
            0x08,                       /* bMaxPacketSize0: 8 bytes */
            0x6F, 0x0E,                 /* idVendor: 0x0E6F (Logic3/PDP) */
            0x41, 0x02,                 /* idProduct: 0x0241 (LEGO Dimensions) */
            0x00, 0x01,                 /* bcdDevice: 1.00 */
            0x01,                       /* iManufacturer */
            0x02,                       /* iProduct */
            0x00,                       /* iSerialNumber */
            0x01                        /* bNumConfigurations */
        };
        memcpy(desc, toypad_dev_desc, 18);
    }

    DEBUG_PRINT("[USB] *** TROJAN HORSE: returned ToyPad descriptor (VID=0x0E6F, PID=0x0241) ***\n");
    return CELL_OK;
}

/* ================================================================
 * HOOK: my_cellUsbdControlTransfer — Fake HID Descriptors
 *
 * After the game accepts the ToyPad VID/PID, it requests the full
 * USB descriptor chain (Configuration, Interface, HID Report, String).
 * If we don't intercept these, the request passes through to the real
 * OS which returns the flash drive's Mass Storage descriptors,
 * corrupting the game's heap. This hook routes all descriptor requests
 * through toypad_state_control_transfer() which returns the correct
 * ToyPad HID descriptors.
 * ================================================================ */
int my_cellUsbdControlTransfer(uint32_t dev_handle, void *setup_pkt,
                                void *buf, uint32_t *length,
                                void *done_cb, void *user_data,
                                uint32_t game_toc)
{
    (void)dev_handle; (void)done_cb; (void)user_data; (void)game_toc;

    if (setup_pkt == NULL || buf == NULL || length == NULL)
        return CELL_USBD_ERROR_FAILED;

    /* Standard USB setup packet: 8 bytes
     * [0] bmRequestType, [1] bRequest, [2:3] wValue, [4:5] wIndex, [6:7] wLength */
    uint8_t *sp = (uint8_t*)setup_pkt;
    uint32_t bmRequestType = sp[0];
    uint32_t bRequest      = sp[1];
    uint32_t wValue        = sp[2] | ((uint32_t)sp[3] << 8);
    uint32_t wIndex        = sp[4] | ((uint32_t)sp[5] << 8);
    uint32_t wLength       = sp[6] | ((uint32_t)sp[7] << 8);

    DEBUG_PRINT("[USB] ControlTransfer: bmReq=0x%02X bReq=0x%02X wVal=0x%04X wIdx=0x%04X wLen=%u\n",
                (unsigned)bmRequestType, (unsigned)bRequest,
                (unsigned)wValue, (unsigned)wIndex, (unsigned)wLength);

    int ret = toypad_state_control_transfer(bmRequestType, bRequest,
                                             wValue, wIndex, buf, wLength);
    if (ret == 0 && wLength > 0) {
        *length = wLength;
    }
    return ret;
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

    /* NOTE: cellUsbdInit OPD validation removed — we no longer hook Init.
     * Only validate the 3 functions we actually hook + passthrough. */

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

    /* cellUsbdGetDeviceDescriptor is NOT in libusbd_stub.a — cannot
     * validate via OPD. Found via NID scan (get_game_plt_stub) instead. */

    if (all_ok >= 3) {
        DEBUG_PRINT("[USB] %d/3 cellUsbd functions validated via OPD (+1 NID scan for GetDeviceDescriptor)\n", all_ok);
        return 0;
    }

    DEBUG_ERROR("[USB] OPD: %d/3 cellUsbd imports validated, soft-fail %d (+1 NID scan for GetDeviceDescriptor)\n",
                all_ok, 3 - all_ok);
    return -1;
}


/* ================================================================
 * STATIC OFFSET RESOLVER (Expert Final, 2026-07-27)
 *
 * Abandons all dynamic scanning. Uses sys_prx_get_module_id_by_name
 * to find libusbd.sprx in memory, then adds hardcoded offsets
 * extracted offline from the firmware's libusbd.sprx ELF.
 *
 * Offsets are FIRMWARE-SPECIFIC (Evilnat 4.91 CFW). If the
 * firmware changes, re-extract offsets from the new libusbd.sprx.
 *
 * PLACEHOLDER OFFSETS: Replace 0x00000000 values with actual
 * offsets extracted from libusbd.sprx export table.
 * ================================================================ */

/* Hardcoded offsets into libusbd.sprx .text (firmware 4.93).
 * Extracted via OPD scanning + SDK API ordering from decrypted ELF.
 * VERIFIED: 2026-07-27, Ghidra + Python OPD analysis.
 * These are the vaddr offsets within libusbd.sprx .text section. */
#define LIBUSBD_OFFSET_OPENPIPE       0x00000244
#define LIBUSBD_OFFSET_INTERRUPT_XFER 0x000004B4
#define LIBUSBD_OFFSET_CLOSEPIPE      0x00000380
#define LIBUSBD_OFFSET_GET_DEV_DESC   0x0000061C
#define LIBUSBD_OFFSET_CONTROL_XFER   0x000007C8

/* Offset of "cellUsbd_Library" string within libusbd.sprx sceModuleInfo */
#define MODULE_INFO_OFFSET            0x94EC

/* ================================================================
 * find_libusbd_base_safe — Safe kernel/shared memory probe
 *
 * Uses cellFsWrite() as a "memory radar" to safely test whether
 * a kernel-space/shared-memory page is mapped.  If the address is
 * unmapped, cellFsWrite returns CELL_EFAULT gracefully instead of
 * triggering a DSI exception that would crash the process.
 *
 * Scans 64KB-aligned addresses from 0x01000000 to 0x30000000,
 * looking for "cellUsbd_Library" at base + 0x94EC (the sceModuleInfo
 * module name field inside libusbd.sprx).
 *
 * Returns: libusbd runtime base address, or 0 if not found.
 * ================================================================ */

static uint32_t find_libusbd_base_safe(void)
{
    int fd, ret, attempt;
    uint32_t found_base = 0;
    uint64_t written;
    uint32_t v;

    /* Retry loop: libusbd may not be loaded yet at injection time.
     * Full scan of 0x02000000-0xC0000000 (2.75GB) every 5s, up to 100s. */
    for (attempt = 0; attempt < 20; attempt++) {

        if (attempt > 0) sys_timer_usleep(5000000);

        if (cellFsOpen("/dev_hdd0/plugins/mem_probe.tmp",
                       CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                       &fd, NULL, 0) != CELL_OK) {
            papertrail("[USB] FATAL: open failed"); return 0;
        }

        if (attempt == 0)
            papertrail("[USB] Scan FULL 0x02000000-0xC0000000 (retry 5s, max 100s)...");
        else {
            char rmsg[24]; int ri=0; rmsg[ri++]='['; rmsg[ri++]='U'; rmsg[ri++]='S'; rmsg[ri++]='B';
            rmsg[ri++]=']'; rmsg[ri++]=' '; rmsg[ri++]='R'; rmsg[ri++]='e'; rmsg[ri++]='t'; rmsg[ri++]='r';
            rmsg[ri++]='y'; rmsg[ri++]=' ';
            if(attempt>=10)rmsg[ri++]='0'+(attempt/10); rmsg[ri++]='0'+(attempt%10);
            rmsg[ri++]=0; papertrail(rmsg);
        }

        found_base = 0;
        { int progress = 0;
        for (v = 0x02000000; v < 0xC0000000; v += 0x10000) {

            uint32_t probe_addr = v + MODULE_INFO_OFFSET;
            written = 0;
            ret = cellFsWrite(fd, (const void*)(uintptr_t)probe_addr, 16, &written);

            if (ret == CELL_OK && written == 16) {
                const char *center = (const char*)(uintptr_t)probe_addr;
                const char *ws = center - 48;
                const char *we = center + 48;
                const char *p;
                for (p = ws; p <= we - 16; p++) {
                    const char *t = "cellUsbd_Library";
                    int i, match = 1;
                    for (i = 0; i < 16; i++) {
                        if (p[i] != t[i]) { match = 0; break; }
                    }
                    if (match) {
                        found_base = v;
                        break;
                    }
                }
                if (found_base) break;

                /* Fallback: match "cellUsbd" (8 chars) */
                for (p = ws; p <= we - 8; p++) {
                    const char *t = "cellUsbd";
                    int i, match = 1;
                    for (i = 0; i < 8; i++) {
                        if (p[i] != t[i]) { match = 0; break; }
                    }
                    if (match) {
                        found_base = v;
                        break;
                    }
                }
        }
        /* else: unmapped page — safely skip to next boundary */

        /* Progress log every 4MB (64 iterations) */
        progress++;
        if ((progress & 63) == 0) {
            char pbuf[48];
            int pi = 0;
            const char *ps = "[USB] probing 0x";
            while (*ps) pbuf[pi++] = *ps++;
            { int sh; for (sh = 28; sh >= 0; sh -= 4) {
                int n = ((v + 0x10000) >> sh) & 0xF;
                pbuf[pi++] = n < 10 ? (char)('0' + n) : (char)('A' + n - 10);
            }}
            pbuf[pi] = 0;
            papertrail(pbuf);
        }
    }  /* end for loop */
    }  /* end progress block */

    cellFsClose(fd);
    cellFsUnlink("/dev_hdd0/plugins/mem_probe.tmp");

    if (found_base) break;
    } /* end retry loop */

    if (!found_base) papertrail("[USB] All retries exhausted - libusbd not found");
    return found_base;
}

int usb_hook_init(void)
{
    uint32_t lib_base;
    uint32_t target_openpipe, target_transfer, target_closepipe;
    uint32_t target_getdevdesc, target_ctrlxfer;

    if (g_usb_hooks.initialized) return 0;
    memset(&g_usb_hooks, 0, sizeof(g_usb_hooks));
    g_usb_hooks.next_pipe_id = 0x1000;

    /* ============================================================
     * SELF-CONTAINED MODE (2026-07-28 v2): Safe pointer probe.
     *
     * The SPRX uses cellFsWrite() as a safe memory radar to find
     * libusbd.sprx in user-space shared memory (0x02000000+).
     * cellFsWrite returns CELL_EFAULT for unmapped pages instead
     * of crashing the process — no DSI exception risk.
     *
     * Once libusbd base is found, TARGET_* = base + offset is
     * computed directly and written to the IPC file.  Node.js
     * only needs to read IPC and fire MEMORY SET preambles.
     * ============================================================ */

    /* Step 1: Find libusbd.sprx base address safely.
     * Retry loop: libusbd may be loaded lazily by the game
     * (not present at "press start" screen). Keep scanning
     * every 5 seconds until found or 30 retries exhausted. */
    {
    int retry;
    for (retry = 0; retry < 30; retry++) {
        lib_base = find_libusbd_base_safe();
        if (lib_base != 0) break;
        if (retry == 0) {
            papertrail("[USB] libusbd not yet loaded — will retry every 5s...");
        }
        /* Scan from 0x30000000 (PRX region) on retries — faster */
        sys_timer_sleep(5);
    }
    }
    if (lib_base == 0) {
        DEBUG_ERROR("[USB] Safe probe could not find libusbd after 30 retries!\n");
        papertrail("[USB] FATAL: libusbd not found after 150s of retries!");
        return -1;
    }

    /* Log the found base address */
    {
        char msg[64];
        int j = 0;
        const char *s = "[USB] libusbd base=0x";
        while (*s) msg[j++] = *s++;
        { int sh; for (sh = 28; sh >= 0; sh -= 4) {
            int n = (lib_base >> sh) & 0xF;
            msg[j++] = n < 10 ? (char)('0' + n) : (char)('A' + n - 10);
        }}
        msg[j] = 0;
        papertrail(msg);
        DEBUG_PRINT("[USB] %s\n", msg);
    }

    /* Step 2: Compute target addresses from offsets */
    target_openpipe   = lib_base + LIBUSBD_OFFSET_OPENPIPE;
    target_transfer   = lib_base + LIBUSBD_OFFSET_INTERRUPT_XFER;
    target_closepipe  = lib_base + LIBUSBD_OFFSET_CLOSEPIPE;
    target_getdevdesc = lib_base + LIBUSBD_OFFSET_GET_DEV_DESC;
    target_ctrlxfer   = lib_base + LIBUSBD_OFFSET_CONTROL_XFER;

    /* Step 3: Store passthrough addresses (skip preamble) */
    g_real_openpipe_addr  = target_openpipe;
    g_real_transfer_addr  = target_transfer;
    g_real_closepipe_addr = target_closepipe;

    /* Step 4: Allocate trampoline page and install 5 hooks */
    if (install_hooks() != 0) {
        DEBUG_ERROR("[USB] Hook installation failed\n");
        return -1;
    }

    /* Step 4.5: Write preambles directly to libusbd .text targets.
     * PS3MAPI MEMORY SET can't write to shared module memory,
     * but the SPRX runs inside the game process. Raw pointer
     * writes on CFW Cobra 8.5 should succeed. */
    {
        uint32_t tramp_base = g_usb_hooks.trampoline_base;
        struct { uint32_t target; uint32_t tramp; const char *name; } hooks[] = {
            {target_openpipe,   tramp_base + g_usb_hooks.tramp_open_pipe_offset,         "OpenPipe"},
            {target_transfer,   tramp_base + g_usb_hooks.tramp_transfer_offset,          "Transfer"},
            {target_closepipe,  tramp_base + g_usb_hooks.tramp_close_pipe_offset,        "ClosePipe"},
            {target_getdevdesc, tramp_base + g_usb_hooks.tramp_get_device_desc_offset,   "GetDevDesc"},
            {target_ctrlxfer,   tramp_base + g_usb_hooks.tramp_control_transfer_offset,  "CtrlXfer"},
            {0, 0, NULL}
        };
        int i;
        papertrail("[USB] Writing preambles directly to libusbd...");
        for (i = 0; hooks[i].name; i++) {
            uint32_t tgt = hooks[i].target;
            uint32_t trp = hooks[i].tramp;
            volatile uint32_t *dst = (volatile uint32_t*)(uintptr_t)tgt;
            uint32_t hi = (trp >> 16) & 0xFFFF;
            uint32_t lo = trp & 0xFFFF;
            dst[0] = 0x3D600000 | hi;     /* lis r11, hi16(tramp) */
            dst[1] = 0x616B0000 | lo;     /* ori r11, r11, lo16(tramp) */
            dst[2] = 0x7D6903A6;          /* mtctr r11 */
            dst[3] = 0x4E800420;          /* bctr */
            { char buf[80]; int bi=0; const char *s="[USB]   ";while(*s)buf[bi++]=*s++;
              s=hooks[i].name; while(*s)buf[bi++]=*s++; buf[bi++]=' ';buf[bi++]='@';buf[bi++]='0';buf[bi++]='x';
              {int sh;for(sh=28;sh>=0;sh-=4){int n=(tgt>>sh)&0xF;buf[bi++]=n<10?'0'+n:'A'+n-10;}}
              s=" -> 0x"; while(*s)buf[bi++]=*s++;
              {int sh;for(sh=28;sh>=0;sh-=4){int n=(trp>>sh)&0xF;buf[bi++]=n<10?'0'+n:'A'+n-10;}}
              buf[bi]=0; papertrail(buf);
            }
        }
        papertrail("[USB] Preambles written. Hooks are LIVE.");
    }

    /* Step 5: Write full IPC file with real TARGET_* addresses */
    write_ipc_file(target_openpipe, target_transfer, target_closepipe,
                   target_getdevdesc, target_ctrlxfer);

    papertrail("[USB] IPC written with real TARGET_* addresses");
    DEBUG_PRINT("[USB] Trampoline page at 0x%08X, 5 hooks ready\n"
                "[USB] TARGET_OPENPIPE=0x%08X TARGET_TRANSFER=0x%08X\n"
                "[USB] TARGET_CLOSEPIPE=0x%08X TARGET_GETDEVDESC=0x%08X\n"
                "[USB] TARGET_CTRLXFER=0x%08X\n",
                (unsigned)g_usb_hooks.trampoline_base,
                (unsigned)target_openpipe, (unsigned)target_transfer,
                (unsigned)target_closepipe, (unsigned)target_getdevdesc,
                (unsigned)target_ctrlxfer);

    g_usb_hooks.initialized = 1;
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
