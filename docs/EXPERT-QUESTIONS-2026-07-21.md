# Expert Questions — LD-ToyPad Bridge (2026-07-21)

## Overview

We have a PS3 homebrew SPRX that injects into the LEGO Dimensions game process via PS3MAPI (webMAN MOD). The SPRX uses PowerPC user-space detour hooks to intercept 5 cellUsbd functions. However, the NID scanner currently fails during live tests on hardware. We need expert validation on several design decisions before we can proceed.

**SPRX Build:** Sony SDK 3.40 DUPLEX, `-mprx` flag, `-llv2_stub -lfs_stub -lnet_stub`
**PS3 Firmware:** 4.91.1 CFW (Cobra 8.4)
**webMAN MOD:** 1.47.48c+
**Game:** LEGO Dimensions BLUS31548 (NTSC-U)

---

## QUESTION 1: NID Scanner — Raw Memory Scan vs. PRX Export Table Walk

**Problem:** Our NID scanner searches raw game memory for NID triplets that form the PRX import table, rather than walking the PRX export/import tables properly.

**Code snippet (usb_hooks.c lines 449-485):**
```c
static int scan_for_nid(void *start, uint32_t size, uint32_t target_nid,
                         void **out_addr, uint32_t *out_got_slot,
                         int strict)
{
    uint32_t *words = (uint32_t*)start;
    uint32_t nwords = size / sizeof(uint32_t);

    for (uint32_t i = 0; i <= nwords - 3; i += 3) {
        uint32_t nid      = words[i + 0];
        uint32_t reserved = words[i + 1];
        uint32_t got_ptr  = words[i + 2];

        if (nid != target_nid) continue;
        if (reserved != 0 && reserved > 0x1000) continue;
        if (got_ptr == 0 || got_ptr < 0x00010000 || got_ptr > 0x4FFFFFFF) continue;

        uint32_t *got_slot = (uint32_t*)(uintptr_t)got_ptr;
        uint32_t func_addr = *got_slot;

        if (func_addr == 0 || func_addr > 0x4FFFFFFF) continue;

        if (strict && func_addr < 0x30000000) continue;  // reject PLT stubs

        *out_addr = (void*)(uintptr_t)func_addr;
        if (out_got_slot) *out_got_slot = got_ptr;
        return 0;
    }
    return -1;
}
```

**Scan regions (lines 54-65):**
```c
static const uint32_t g_scan_regions[][2] = {
    { 0x00100000, 0x00400000 },   // game .text + import tables
    { 0x30000000, 0x00100000 },   // system PRX export tables
};
```

**Question:** Is scanning raw memory for NID triplets (NID, reserved, GOT-slot) at 12-byte strides a valid approach for finding PRX import stubs on CellOS? Or must we walk the PRX module list (`sys_prx_get_module_list_by_id`) and parse each module's export/import ELF sections? If the raw scan is unreliable, what is the correct API or memory structure to use?

---

## QUESTION 2: OPD Construction — extern uint32_t vs. Function Pointers

**Problem:** The Sony SDK's `-mprx` fixup pass requires OPD entries for every cross-object function call. Assembly (.s) symbols don't have OPDs, causing "undefined reference" linker errors. Our solution declares assembly symbols as `extern uint32_t` (data objects) and manually constructs OPD structs in C.

**Code snippet (toc_trampoline_c.c lines 38-84):**
```c
/* Declared as data objects, NOT functions! */
extern uint32_t asm_wrapper_my_cellUsbdInit;
extern uint32_t asm_passthrough_OpenPipe;

typedef struct {
    uint32_t code_addr;    /* Ptr to .text code */
    uint32_t toc_addr;     /* TOC base (loaded into r2 on call) */
    uint32_t env_ptr;      /* Environment pointer (unused, set to 0) */
} ppc_opd_t;

/* Manually constructed OPD with toc_addr=0 */
ppc_opd_t g_opd_passthrough_OpenPipe = {
    .code_addr = (uint32_t)(uintptr_t)&asm_passthrough_OpenPipe,
    .toc_addr  = 0,    /* Overwritten by stub immediately */
    .env_ptr   = 0
};

/* Cast OPD address to function pointer type */
passthrough_openpipe_fn call_original_OpenPipe =
    (passthrough_openpipe_fn)&g_opd_passthrough_OpenPipe;
```

**Question 2a:** Is declaring assembly labels as `extern uint32_t` and taking their address to get the raw .text pointer a robust technique with `-mprx`? Will the linker's OPD fixup pass interfere with this pattern?

**Question 2b:** We set `toc_addr = 0` in the OPD because the HOOK_PASSTHROUGH assembly stub immediately overwrites r2 with the game TOC passed in r3. Is there any risk that the CellOS loader crashes when calling through an OPD with a NULL TOC, or does the value only matter after the function prologue?

---

## QUESTION 3: HOOK_WRAPPER — Is the TOC Passing Correct?

**Problem:** Each cellUsbd function has a different number of arguments. The HOOK_WRAPPER macro saves the game's r2 (TOC), then passes it to the C hook as the "last" argument. Since PowerPC r3-r10 are used for arguments, the TOC occupies whatever register comes after the last real argument.

**Code snippet (toc_trampoline.s lines 41-72):**
```asm
.macro HOOK_WRAPPER asm_name, c_function, toc_reg
    .align 2
    .globl \asm_name
    \asm_name:
        stwu %r1, -0x60(%r1)       /* Allocate 0x60-byte frame */
        mflr %r0
        stw  %r0, 0x64(%r1)        /* Save LR */
        stw  %r2, 0x28(%r1)        /* Save game TOC */

        lwz  \toc_reg, 0x28(%r1)   /* Reload TOC into arg register */
        bl   \c_function            /* Call C hook */

        lwz  %r2, 0x28(%r1)        /* Restore game TOC */
        lwz  %r0, 0x64(%r1)        /* Restore LR */
        mtlr %r0
        addi %r1, %r1, 0x60        /* Deallocate frame */
        blr
.endm
```

**Hook wrappers instantiated (lines 138-151):**
```asm
/* cellUsbdInit has 0 args -> TOC in r3 */
HOOK_WRAPPER asm_wrapper_my_cellUsbdInit, my_cellUsbdInit, %r3

/* cellUsbdOpenPipe has 3 args (r3,r4,r5) -> TOC in r6 */
HOOK_WRAPPER asm_wrapper_my_cellUsbdOpenPipe, my_cellUsbdOpenPipe, %r6

/* cellUsbdTransfer has 5 args (r3,r4,r5,r6,r7) -> TOC in r8 */
HOOK_WRAPPER asm_wrapper_my_cellUsbdTransfer, my_cellUsbdTransfer, %r8

/* cellUsbdClosePipe has 1 arg (r3) -> TOC in r4 */
HOOK_WRAPPER asm_wrapper_my_cellUsbdClosePipe, my_cellUsbdClosePipe, %r4

/* cellUsbdRegisterLdd has 1 arg (r3) -> TOC in r4 */
HOOK_WRAPPER asm_wrapper_my_cellUsbdRegisterLdd, my_cellUsbdRegisterLdd, %r4
```

**C hook signatures (usb_hooks.h):**
```c
int my_cellUsbdInit(uint32_t game_toc);                              // 0 args + TOC
int my_cellUsbdOpenPipe(uint32_t *pipe_handle, uint32_t dev_id,       // 3 args + TOC
                         void *ep_descriptor, uint32_t game_toc);
int my_cellUsbdTransfer(uint32_t pipe_handle, void *buf,              // 5 args + TOC
                         uint32_t *len, uint32_t arg4, uint32_t arg5,
                         uint32_t game_toc);
int my_cellUsbdClosePipe(uint32_t pipe_handle, uint32_t game_toc);    // 1 arg + TOC
int my_cellUsbdRegisterLdd(CellUsbdLddOps *ops, uint32_t game_toc);   // 1 arg + TOC
```

**Question:** Is the TOC register mapping correct? Specifically:
- For cellUsbdInit (0 original args): r3 is the TOC, but r3 is also the return value register. When `my_cellUsbdInit` returns CELL_OK in r3, does this stomp on the game's TOC value before the wrapper restores r2?
- For cellUsbdOpenPipe (3 args): original args in r3,r4,r5; TOC goes to r6. Is r6 preserved by the `bl` instruction (the callee's responsibility)?
- Is the HOOK_WRAPPER saving/restoring all non-volatile registers, or just r2 and LR? Should we also save/restore r14-r31 per the ABI?

---

## QUESTION 4: HOOK_PASSTHROUGH — Argument Shifting Correctness

**Problem:** The passthrough stub consumes r3 (game_toc) and r4 (tramp_addr), then shifts r5+ down to r3+. This needs to restore the original argument layout when calling the trampoline (which contains the original function instructions).

**Code snippet (toc_trampoline.s lines 85-130):**
```asm
.macro HOOK_PASSTHROUGH asm_name, num_args
    stwu %r1, -0x60(%r1)
    mflr %r0
    stw  %r0, 0x64(%r1)

    mr   %r2, %r3              /* Restore game TOC from r3 */
    mr   %r11, %r4             /* Save trampoline address from r4 */

    /* Shift r5+ down to r3+ */
    .if \num_args >= 1
    mr   %r3, %r5
    .endif
    .if \num_args >= 2
    mr   %r4, %r6
    .endif
    .if \num_args >= 3
    mr   %r5, %r7
    .endif
    ...etc...

    mtctr %r11                 /* Load trampoline addr into CTR */
    bctrl                      /* Branch-and-link to trampoline */

    lwz  %r0, 0x64(%r1)
    mtlr %r0
    addi %r1, %r1, 0x60
    blr
.endm
```

**Passthrough stubs (lines 158-165):**
```asm
HOOK_PASSTHROUGH asm_passthrough_OpenPipe,   3
HOOK_PASSTHROUGH asm_passthrough_Transfer,   5
HOOK_PASSTHROUGH asm_passthrough_ClosePipe,  1
```

**Call sites in C (usb_hooks.c lines 318-321 for example):**
```c
return call_original_OpenPipe(game_toc,
    (void*)(uintptr_t)g_usb_hooks.tramp_open_pipe_addr,
    pipe_handle, dev_id, ep_descriptor);
```

**Question 4a:** For ClosePipe (1 arg), the C call pushes:
- r3 = game_toc
- r4 = tramp_addr (via tramp_open_pipe_addr)
- r5 = original pipe_handle

The stub does `mr r2, r3` (restore TOC), `mr r11, r4` (save tramp addr), `mr r3, r5` (shift arg down), then `bctrl` to trampoline. The trampoline (4 saved original insns + branch-back to target+16) then returns. After `bctrl` returns, the stub restores LR and `blr` to the C caller. Is this correct?

**Question 4b:** `bctrl` sets the link register (LR) to the return address inside the stub. After the trampoline executes the 4 original instructions and branches back to target+16, when the original function eventually returns, will it return correctly to our stub's `bctrl` return point? Or will the original function's `blr` restore an LR that points back to the game's caller, bypassing our stub?

---

## QUESTION 5: NID Values — Verification Required

**Problem:** We need confirmation that these NID values are correct for cellUsbd functions on firmware 4.91.

**Hardcoded in usb_hooks.c lines 46-50:**
```c
#define NID_CELL_USBD_INIT              0x7F5F00D3U
#define NID_CELL_USBD_OPEN_PIPE         0x1AB6D80BU
#define NID_CELL_USBD_TRANSFER          0x7B4436CEU
#define NID_CELL_USBD_CLOSE_PIPE        0x2F82F1A5U
#define NID_CELL_USBD_REGISTER_LDD      0xD9B4C7A2U
```

**Question:** Can you confirm these NID values are correct for firmware 4.91? If not, what are the correct values or how can we compute them (SHA-1 of the function name pattern)? We extracted these from various open-source PS3 projects but they may be firmware-specific.

---

## QUESTION 6: SO_NBIO Non-blocking Mode Reliability

**Problem:** We use `setsockopt(sock, SOL_SOCKET, SO_NBIO, ...)` to set non-blocking mode. There is a concern that MSG_DONTWAIT is unreliable on CellOS' `-lnet_stub`.

**Code snippet (network.c lines 136-143):**
```c
{
    int optval = 1;  /* SO_NBIO: 0 = blocking, 1 = non-blocking */
    if (setsockopt(g_net.socket_fd, SOL_SOCKET, SO_NBIO,
                   (void*)&optval, sizeof(optval)) < 0) {
        DEBUG_ERROR("[NET] setsockopt SO_NBIO failed\n");
    }
}
```

**Recvfrom call (network.c line 364):**
```c
ret = recvfrom(g_net.socket_fd, buffer, (size_t)buf_size, 0, ...);
/* flags=0, relying on SO_NBIO for non-blocking behavior */
```

**Question:** 
- Does SO_NBIO actually make the socket non-blocking on CellOS SDK 3.40?
- Is there a way to verify it was applied (e.g., `getsockopt` to read it back)?
- Are there any edge cases where SO_NBIO is ignored (e.g., certain socket states)?

---

## QUESTION 7: sys_memory_allocate — Are Pages R-W-X?

**Problem:** We need pages that are simultaneously Readable, Writable, AND Executable for our trampolines.

**Code (usb_hooks.c lines 93-126):**
```c
static int allocate_trampolines(void)
{
    sys_memory_container_t container = SYS_MEMORY_CONTAINER_DEFAULT;  // 0xFFFFFFFF
    uint32_t base_addr;
    int ret = sys_memory_allocate(TRAMPOLINE_PAGE_SIZE,       // 64KB
                                   SYS_MEMORY_PAGE_SIZE_64K,
                                   &base_addr);
    // ...use base_addr as R-W-X...
}
```

**Question:** Does `sys_memory_allocate` guarantee the returned page is simultaneously readable, writable, AND executable on CellOS 3.40+? Some Sony documentation suggests memory allocated via this API is not executable by default, and you need `sys_memory_map()` with specific protection flags. If `sys_memory_allocate` alone doesn't give us R-W-X, what is the correct API sequence to obtain an executable trampoline page from user-space (not kernel)?

---

## QUESTION 8: SPRX Signing Type — SELF (Application) vs. ISLAND

**Problem:** Our SPRX is signed with `-0 SELF -5 APP` flags (application type SELF).

**oscetool flags from Makefile (line 31-37):**
```
-0 SELF -1 TRUE
-2 000A
-3 1010000001000003
-4 01000002
-A 0001000000000000 -5 APP
-8 4000000000000000000000000000000000000000000000000000000000000002
-6 0004009300000000
```

**Question:** Is `TYPE=SELF, APP` the correct signing format for a PRX that will be injected into a game process via PS3MAPI's `MODULE LOAD` command? Or should it be signed as `TYPE=ISLAND` (the standard for PS3 PRX/SPRX files)? We previously had a working hello-plugin SPRX signed as ISLAND that loaded successfully. Could using the wrong type cause `MODULE LOAD` to return success but silently fail to initialize the module?

---

## QUESTION 9: module_start return value for Game Process Injection

**Problem:** When the SPRX is loaded via PS3MAPI `MODULE LOAD` into a running game process, we need to understand the correct return value conventions.

**Code (main.c lines 63-132):**
```c
int module_start(size_t args, void *argp)
{
    // ...
    sys_ppu_thread_create(&g_worker_tid, worker_thread, 0, 1000, 16*1024, ...);
    return 0;   /* Return 0 = module loaded successfully */
}

int module_stop(void)
{
    g_shutdown = 1;
    sys_ppu_thread_join(g_worker_tid, &ev);
    // cleanup...
    return SYS_PRX_STOP_OK;
}
```

**Question:** 
- Does returning 0 from `module_start` correctly signal to PS3MAPI's `MODULE LOAD` that the module was loaded, even when injected into an already-running process?
- We also have a VSH guard that returns `SYS_PRX_STOP_OK` to abort loading if not in the game process. Is this the correct return value to tell the kernel "unload me immediately"?

---

## QUESTION 10: Heartbeat Counter in sys_memory_allocate Page

**Problem:** We store a heartbeat counter at offset 160 in the sys_memory_allocate page.

**Code (usb_hooks.c line 119):**
```c
g_usb_hooks.heartbeat = (volatile uint32_t*)(uintptr_t)(base_addr + 160);
```

**Increment from main.c worker loop (line 313):**
```c
if (g_usb_hooks.heartbeat) {
    (*g_usb_hooks.heartbeat)++;
}
```

**Question:** Is there any cache coherency issue reading this value from the Node.js side via PS3MAPI's `MEMORY READ` (or `/read_process`)? CellOS has split L1 caches (PPU has separate data/instruction caches). Do we need an `dcbf` (data cache block flush) after writing the heartbeat, or will the PS3MAPI kernel module read from physical memory directly (bypassing the PPU's L1)?

---

## Summary

The SPRX builds successfully (113KB PRX → 18KB signed SPRX), deploys to PS3, and gets injected. The `network_init()` succeeds and UDP works. However, the **NID scanner fails to find the 5 cellUsbd function addresses**, which prevents the IPC file from being written and the preambles from being installed.

Our primary suspicion is that either:
1. The NID values are wrong for firmware 4.91
2. The raw memory scan regions don't contain the NID triplets in the expected format
3. The two-phase scanner (strict → relaxed) has a logic bug

We would greatly appreciate expert guidance on any/all of these questions.
