# LD-ToyPad Bridge — CRITICAL HANDOFF REPORT
## Date: 2026-07-20 09:30 UTC-6
## Status: Path 3 Implementation INCOMPLETE — 5 Critical Bugs Block All Functionality

---

# EXECUTIVE SUMMARY

The project has made excellent progress on the non-functional parts (networking, initialization, discovery protocol), but the **core hooking mechanism has 5 critical bugs** that will cause the PS3 to crash or the hooks to do nothing. **No Toy Pad functionality works yet.**

---

# WHAT IS WORKING (confirmed by boot logs)

1. **SPRX init chain** — clean boot every time:
   - debug_init → network_init → network_wait_ready → toypad_state_init → main loop
2. **Network keepalive** — PS3 sends `0xEE` heartbeat to server every 3s
3. **Server registration** — Node.js server receives keepalive and tracks `clientAddress`
4. **Discovery protocol** — Self-loop rejection fixed (PS3 ignores own broadcasts), 0xF0 magic byte
5. **Server hardcoded** — `network_set_server(0xC0A80011, 28472)` bypasses broadcast issues
6. **inject-sprx.js** — PS3MAPI detection of LEGO Dimensions PID, waits 60s, injects SPRX via HTTP API
7. **NID scanner** — Finds cellUsbd import stubs in game memory (lis/ori patterns for 4 known NIDs)
8. **toypad_state.c** — USB descriptors compiled, state machine initialized
9. **spoofed descriptors** — Device, Configuration, HID Report, String descriptors defined

---

# THE 5 CRITICAL BUGS (must fix, in order)

## BUG 1: `hook_install()` preamble uses `ba` — cannot reach hook functions
**File:** `sprx-plugin/hook.c:168`
**Severity:** CRITICAL — WILL CRASH PS3

The current code writes `target[0] = hook_build_ba(ctx->hook_fn)`. PowerPC `ba` instruction only has 24 bits for the address (LI field = bits 6-29), giving a **±32MB range** from bit 0. The game's cellUsbd functions are at ~0x00100000, but the SPRX could be mapped at 0x30000000+. That's 768MB apart. The `ba` will jump to garbage.

**Fix:** Replace with 4-instruction preamble:
```
[0] mflr r0           (save link register)
[1] stw r2, 8(r1)     (save game TOC on stack)
[2] lis r11, hi       (load high 16 bits of hook fn address)
[3] ori r11, r11, lo  (load low 16 bits)
```
Then at target+16 (4 bytes after patch):
```
[4] mtctr r11
[5] bctr              (branch to hook function)
```

**Ready-to-use replacement code for hook.h and hook.c is provided below.**

## BUG 2: TOC (r2) NOT SAVED — game will crash after hook returns
**File:** `sprx-plugin/hook.c:149-172`
**Severity:** CRITICAL — WILL CRASH PS3

When the game's import stub branches to our code, r2 contains the **game's TOC**. Our Sony SDK `-mprx` prologue changes r2 to our PRX's TOC. Per PowerPC calling convention, r2 is **callee-preserved**. When our hook returns, the game tries to use r2, gets our PRX TOC, and crashes.

**Fix:** The preamble must save r2 onto the stack (done in Bug 1 fix), and every hook function must restore r2 before returning:
```c
// In hook function entry (as first C statement)
__asm__ volatile("lwz r2, 8(r1)");  // restore game TOC
```

## BUG 3: `scan_for_nid()` returns NULL addresses — hooks never installed
**File:** `sprx-plugin/usb_hooks.c:597`
**Severity:** CRITICAL — Nothing Works

Line 597: `*out_addr = NULL; /* Will be resolved in production */`

The NID scanner correctly finds import stubs in game memory but **sets the function address to NULL**. This means `hook_install()` receives NULL target addresses, and `usb_hook_init()` returns -1.

**Fix:** The NID scanner needs to resolve the actual GOT address. When the scanner finds the `lis/ori` pattern followed by `lwz r12, offset(r12)`, it needs to:
1. Parse the offset from the `lwz` instruction (bits 16-31 of opcode `0x818CXXXX`)
2. Calculate the GOT slot address = (NID value from lis/ori << 16) + offset
3. Read the 32-bit value at that GOT slot address — that's the actual function pointer

**Ready-to-use replacement code for scan_for_nid() provided below.**

## BUG 4: Non-Toy-Pad USB passthrough will TOC-crash
**File:** `sprx-plugin/usb_hooks.c:291-293`
**Severity:** HIGH — Non-Toy-Pad USB (controllers, storage) will crash

When `my_cellUsbdOpenPipe` calls through the trampoline for non-Toy-Pad pipes:
```c
tramp_fn_t real_open = (tramp_fn_t)(void*)&g_usb_hooks.tramp_open_pipe;
return real_open(pipe_handle, dev_id, ep_descriptor);
```
The trampoline contains the original cellUsbd function's instructions, which make an LV2 syscall. The original function expects **game TOC in r2**, but r2 contains our PRX's TOC. Crash.

**Fix:** Restore game TOC before calling through trampoline:
```c
__asm__ volatile("lwz r2, 8(r1)");  // restore game TOC
return real_open(pipe_handle, dev_id, ep_descriptor);
```

## BUG 5: `hook_build_ba()` wrong bit handling
**File:** `sprx-plugin/hook.h:126-131`
**Severity:** MEDIUM — but this code will be removed by Bug 1 fix anyway

---

# READY-TO-USE REPLACEMENT CODE

## 1. Complete replacement for `sprx-plugin/hook.h`

```c
/**
 * hook.h — PowerPC User-Space Detour Hooking (Sony SDK)
 *
 * FIXED: Replaced ba-only preamble (limited to +-32MB) with lis/ori/mtctr/bctr
 * that can reach any 32-bit address. Added proper TOC (r2) save/restore.
 */
#ifndef HOOK_H
#define HOOK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HOOK_NUM_INSTRUCTIONS  4
#define HOOK_TRAMPOLINE_SIZE   (5 * 4)
typedef struct {
    uint32_t instructions[HOOK_NUM_INSTRUCTIONS];
    uint32_t branch_back;
} hook_trampoline_t;
typedef struct {
    void *target_addr;
    void *hook_fn;
    hook_trampoline_t *tramp;
} hook_ctx_t;
int hook_install(hook_ctx_t *ctx);
int hook_remove(hook_ctx_t *ctx);
void hook_flush_icache(void *addr, uint32_t size);
static inline uint32_t hook_build_nop(void) { return 0x60000000; }
static inline uint32_t hook_build_mflr_r0(void) { return 0x7C0802A6; }
static inline uint32_t hook_build_stw_r2(void) { return 0x90410008; }
static inline uint32_t hook_build_lwz_r2(void) { return 0x80410008; }
static inline uint32_t hook_build_lis_r11(uint32_t v) { return 0x3D600000 | (v & 0xFFFF); }
static inline uint32_t hook_build_ori_r11(uint32_t v) { return 0x616B0000 | (v & 0xFFFF); }
static inline uint32_t hook_build_mtctr_r11(void) { return 0x7D6B03A6; }
static inline uint32_t hook_build_bctr(void) { return 0x4E800420; }
static inline void hook_build_load_r11(uint32_t val, uint32_t out[2]) {
    out[0] = hook_build_lis_r11(val >> 16);
    out[1] = hook_build_ori_r11(val & 0xFFFF);
}
#ifdef __cplusplus
}
#endif
#endif
```

## 2. Complete replacement for `sprx-plugin/hook.c`

```c
/**
 * hook.c — PowerPC User-Space Detour Hooking Engine (Sony SDK)
 *
 * FIXED: Replaced ba-only approach (limited to +-32MB) with lis/ori/mtctr/bctr
 * preamble that loads ANY 32-bit address. Added TOC save/restore.
 *
 * Preamble at target[0..3]:
 *   [0] mflr r0          (save link register)
 *   [1] stw  r2, 8(r1)   (save game TOC onto stack)
 *   [2] lis  r11, hi     (load high 16 bits of hook addr)
 *   [3] ori  r11, r11, lo (load low 16 bits)
 * Then at target[4..5]:
 *   [4] mtctr r11
 *   [5] bctr
 */
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "hook.h"
#include "debug.h"

static inline void ppu_dcbst(void *addr) {
    __asm__ volatile ("dcbst 0, %0; sync; icbi 0, %0; isync" :: "r"(addr) : "memory");
}
static inline void ppu_sync(void) { __asm__ volatile ("sync" ::: "memory"); }
static inline void ppu_icbi(void *addr) { __asm__ volatile ("icbi 0, %0" :: "r"(addr) : "memory"); }
static inline void ppu_isync(void) { __asm__ volatile ("isync"); }

void hook_flush_icache(void *addr, uint32_t size) {
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)0x7F;
    uintptr_t end = (uintptr_t)addr + size;
    while (a < end) { ppu_dcbst((void*)a); a += 128; }
    ppu_sync();
    a = (uintptr_t)addr & ~(uintptr_t)0x7F;
    while (a < end) { ppu_icbi((void*)a); a += 128; }
    ppu_isync();
}

int hook_install(hook_ctx_t *ctx) {
    uint32_t *target;
    uint32_t hook_addr;
    int i;

    if (!ctx || !ctx->target_addr || !ctx->hook_fn || !ctx->tramp) {
        DEBUG_ERROR("[HOOK] hook_install: invalid args\n");
        return -1;
    }

    target = (uint32_t*)ctx->target_addr;
    hook_addr = (uint32_t)(uintptr_t)ctx->hook_fn;

    /* Step 1: Save original 4 instructions into trampoline */
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++)
        ctx->tramp->instructions[i] = target[i];

    /* Step 2: Build branch-back at trampoline[4] */
    {
        uint32_t back_addr = (uint32_t)(uintptr_t)ctx->target_addr + (HOOK_NUM_INSTRUCTIONS * 4);
        ctx->tramp->branch_back = 0x48000002 | ((back_addr >> 2) & 0x03FFFFFC);
    }

    /* Step 3: NOP safe window */
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++)
        target[i] = hook_build_nop();
    hook_flush_icache(ctx->target_addr, HOOK_NUM_INSTRUCTIONS * 4);

    /* Step 4: Install preamble
     *   [0] mflr r0       — save LR
     *   [1] stw r2, 8(r1) — save game TOC
     *   [2] lis r11, hi   — load hook addr high
     *   [3] ori r11, lo   — load hook addr low
     */
    target[0] = hook_build_mflr_r0();
    target[1] = hook_build_stw_r2();
    hook_build_load_r11(hook_addr, &target[2]);

    /* Step 4b: Write mtctr/bctr at target+16 */
    {
        uint32_t *patch_ext = (uint32_t*)((uint8_t*)target + (HOOK_NUM_INSTRUCTIONS * 4));
        patch_ext[0] = hook_build_mtctr_r11();
        patch_ext[1] = hook_build_bctr();
    }

    /* Step 5: Flush icache (24 bytes patched) */
    hook_flush_icache(ctx->target_addr, (HOOK_NUM_INSTRUCTIONS + 2) * 4);
    hook_flush_icache(ctx->tramp, HOOK_TRAMPOLINE_SIZE);

    DEBUG_PRINT("[HOOK] Installed: target=0x%08X -> hook=0x%08X\n",
                (unsigned)(uintptr_t)ctx->target_addr, hook_addr);
    return 0;
}

int hook_remove(hook_ctx_t *ctx) {
    uint32_t *target;
    int i;
    if (!ctx || !ctx->target_addr || !ctx->tramp) return -1;
    target = (uint32_t*)ctx->target_addr;
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++)
        target[i] = hook_build_nop();
    hook_flush_icache(ctx->target_addr, HOOK_NUM_INSTRUCTIONS * 4);
    for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++)
        target[i] = ctx->tramp->instructions[i];
    hook_flush_icache(ctx->target_addr, HOOK_NUM_INSTRUCTIONS * 4);
    DEBUG_PRINT("[HOOK] Removed: restored target at 0x%08X\n",
                (unsigned)(uintptr_t)ctx->target_addr);
    return 0;
}
```

## 3. Fix for `scan_for_nid()` in `usb_hooks.c`

Replace the loop body in `scan_for_nid()` (the section after finding lis/ori, lines ~556-601) with:

```c
        /* Found potential import stub.
         * Look for lwz r12, offset(r12) in the next 4-8 instructions.
         * Pattern: 0x818CXXXX where XXXX is the GOT slot offset.
         * The GOT slot is at: (nid_high << 16) + offset
         * We read the function pointer from that GOT slot directly. */
        
        /* Search up to 8 instructions forward for lwz pattern */
        for (int j = 2; j < 10; j++) {
            if ((insn_ptr[j] & 0xFFFF0000) == 0x818C0000) {
                /* Found lwz r12, offset(r12) */
                uint32_t got_offset = insn_ptr[j] & 0xFFFF;
                uint32_t got_base = nid_high << 16;  /* lis sets r12 to this */
                uint32_t got_addr = got_base + got_offset;
                
                /* Dereference GOT slot to get function address */
                void **got_slot = (void**)(uintptr_t)got_addr;
                *out_addr = *got_slot;
                
                DEBUG_PRINT("[USB] NID 0x%08X resolved via GOT 0x%08X -> func=0x%08X\n",
                    (unsigned)target_nid, got_addr, 
                    (unsigned)(uintptr_t)*out_addr);
                return 0;
            }
        }
        
        /* If no lwz found after lis/ori, log and continue scanning */
        DEBUG_VERBOSE("[USB] NID 0x%08X stub at 0x%08X has no adjacent lwz\n",
            (unsigned)target_nid, (unsigned)(uintptr_t)(bytes + off));
        continue;
```

## 4. TOC restore in hook functions

Add this as the first line of each hook function in `usb_hooks.c`:

```c
/* Restore game's TOC (r2) from stack (saved by preamble) */
__asm__ volatile("lwz r2, 8(r1)");
```

Also add before trampoline calls in non-Toy-Pad passthrough paths.

---

# FILES THAT ARE CORRECT (no changes needed)

- `sprx-plugin/main.c` — Dual-mode VSH/game architecture is correct
- `sprx-plugin/network.c` — Self-loop rejection works, keepalive works
- `sprx-plugin/network.h` — Protocol defines are correct
- `sprx-plugin/toypad_state.c` — Descriptors and state machine correct
- `sprx-plugin/toypad_state.h` — Interface is clean
- `sprx-plugin/usb_hooks.h` — Types are correct
- `sprx-plugin/debug.h/c` — Debug subsystem works
- `ld-toypad-server/server.js` — Keepalive handling works
- `ld-toypad-server/inject-sprx.js` — Injection logic works (needs auto-reinjection loop)

---

# QUESTIONS FOR THE EXPERT

1. **GOT resolution safety**: When the NID scanner finds `lwz r12, offset(r12)`, can we safely dereference the GOT slot address = (nid_high << 16) + offset? Or does r12 contain something different at runtime (e.g., a GOT base pointer set by the PRX loader)?

2. **TOC save strategy**: Is `stw r2, 8(r1)` sufficient for saving the game's TOC? The stack frame at r1 (SP) must have at least 8 bytes available. CellOS functions typically reserve 0x70+ bytes at function entry with `stwu r1, -0x70(r1)`. Is it safe to assume 8 bytes are available when our preamble runs?

3. **PS3MAPI URL**: What's the correct URL format? Is it `http://{ip}/ps3mapi?pid={pid}&load_prx={path}` or something like `http://{ip}/ps3mapi_process?pid={pid}&load_prx={path}`? Different CFW versions use different paths.

4. **Memory protection**: After PS3MAPI injects the SPRX into the game process, can we write to the game's .text section (where cellUsbd functions reside) directly via pointer dereference? Or does .text have R-X permissions that need LV2 syscalls to change first?

5. **Target+16 safety**: The fix writes 2 extra instructions (mtctr/bctr) at target+16, which is beyond our 4-instruction patch window. Is this safe? The 5th and 6th instructions of the original function don't need to be preserved since we never return to them. But could they be jump targets from elsewhere in the function?

6. **ba opcode encoding**: The current `hook_build_ba()` returns `0x48000002 | (addr & 0x03FFFFFC)`. Should this be `0x48000002 | ((addr >> 2) & 0x03FFFFFC)` instead? The LI field should contain address/4, not the raw address.

---

# SUMMARY OF TASK REMAINING

| Task | Priority | Files | Est. Effort |
|------|----------|-------|-------------|
| Fix hook preamble (ba→lis/ori/mtctr/bctr) | P0 | hook.c, hook.h | 30 min |
| Fix TOC save/restore | P0 | hook.c, usb_hooks.c | 20 min |
| Fix GOT resolution in scan_for_nid() | P0 | usb_hooks.c | 30 min |
| Fix non-Toy-Pad passthrough TOC | P1 | usb_hooks.c | 10 min |
| Add auto-reinjection loop | P2 | inject-sprx.js | 20 min |
| Wire toypad_state to USB data path | P2 | usb_hooks.c | 15 min |
| Build, deploy, test | P0 | build-all.ps1 | ongoing |

**Total remaining work: ~2 hours for a focused agent**
