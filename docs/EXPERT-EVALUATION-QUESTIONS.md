# Expert Evaluation Questions — Crash-on-Boot Trace Analysis

## Summary

The LD-ToyPad Bridge SPRX causes LEGO Dimensions to crash immediately on boot after applying the 2026-07-20 refactoring (strict 4-instruction preamble with assembly TOC trampoline). Below is a layer-by-layer trace of every code path that executes, with identified potential failure points.

## Crash Hypothesis: Primary Suspects

### 1. Direct Memory Write to R-X .text Segment (CRITICAL — Most Likely Cause)

**Location:** `hook.c` lines 137-139 and 158-160

```c
// hook_install hook.c:137-139 (Step 3: NOP safe window)
for (i = 0; i < HOOK_NUM_INSTRUCTIONS; i++) {
    target[i] = hook_build_nop();  // DIRECT WRITE
}

// hook_install hook.c:158-160 (Step 4: Preamble install)
hook_build_load_r11(hook_addr, &target[0]);
target[2] = hook_build_mtctr_r11();
target[3] = hook_build_bctr();
```

The CellOS .text segment is marked **R-X (Read-Execute, no Write)**. The handoff document (Defect 1, Memory Protection Note) explicitly states:

> *"Memory Protection Note: The CellOS .text segment is marked Read-Execute (R-X). Standard pointer dereferencing will cause a Data Storage Interrupt (DSI). The four-instruction patch must be installed utilizing the LV2 syscall sys_process_write_memory (or equivalent PS3MAPI memory write endpoints)."*

**Current `hook.c` does NOT use `sys_process_write_memory` or any memory protection bypass.** It writes directly to the target address via C pointer dereference (`target[i] = ...`). This should immediately cause a **DSI exception** and crash the game the first time `hook_install` runs.

**Questions for Expert:**

> **Q1:** Is the direct pointer write to the game's .text segment definitely the crash cause? Is there any mechanism in the Sony SDK's -mprx build that would allow writes to R-X memory (e.g., the SPRX runs at a privilege level that bypasses page protection)?

> **Q2:** If direct write is the problem, what is the correct mechanism to write to R-X memory from within an SPRX running in the game's user process?
>   - Option A: `sys_process_write_memory` LV2 syscall (which syscall number?)
>   - Option B: `sys_mmapper_allocate_memory` + `sys_mmapper_map_memory` to create a writable alias
>   - Option C: Temporarily change page protection via `sys_mmapper_change_page_access_rights` (syscall number?)
>   - Option D: Use PS3MAPI HTTP endpoints (requires external controller, not available from within SPRX)
>   - Option E: Some other Sony SDK API?

### 2. Trampoline Buffer in Non-Executable Memory (CRITICAL)

**Location:** `usb_hooks.c` line 46, `usb_hooks.h` lines 59-62

```c
// usb_hooks.h:59-62  (in usb_hook_state_t)
hook_trampoline_t tramp_init;
hook_trampoline_t tramp_open_pipe;
hook_trampoline_t tramp_transfer;
hook_trampoline_t tramp_close_pipe;

// usb_hooks.c:834  (in usb_hook_init)
memset(&g_usb_hooks, 0, sizeof(g_usb_hooks));
```

The `g_usb_hooks` global struct (with embedded trampoline buffers) is allocated in the SPRX's .bss section. The SPRX .bss/.data sections are typically **R-W (Read-Write)**, not **R-W-X (Read-Write-Execute)**.

When execution hits the trampoline via `bctr` from the preamble, the PPU will try to fetch instructions from an address in .bss/.data. If that memory is not executable, a **ISI (Instruction Storage Interrupt)** will occur.

**Questions for Expert:**

> **Q3:** Are .data/.bss sections in SPRX modules compiled with `-mprx` executable by default, or must they be explicitly made executable? If not executable, what is the correct approach?
>   - Option A: Allocate trampoline in .text section via `__attribute__((section(".text")))`
>   - Option B: Use `sys_mmapper_allocate_memory` to get executable pages
>   - Option C: Use LV2 syscall to change page protection on the trampoline's page

> **Q4:** If Option A (allocate in .text) is preferred, can we safely mark the trampoline buffer in a C struct with `__attribute__((section(".text")))`? Or does it need to be in a separate `.trampoline` section with appropriate linker script flags?

### 3. NID Scanner Dereferencing Invalid Memory

**Location:** `usb_hooks.c` lines 554-637 (`scan_for_nid`)

The scanner walks memory regions 0x00100000-0x40000000 looking for 12-byte import table entries. When it finds what looks like a valid NID match, it dereferences `got_ptr` at line 613-614:

```c
uint32_t *got_slot = (uint32_t*)(uintptr_t)got_ptr;
uint32_t func_addr = *got_slot;  // dereference
```

If `got_ptr` is a false positive (looks valid but points to unmapped or non-readable memory), this dereference will cause a **DSI**.

**Questions for Expert:**

> **Q5:** Is the 12-byte import table entry format (NID at +0, reserved at +4, got_ptr at +8) correct for Sony SDK PRX modules in LEGO Dimensions (BLUS31548 / BLES02206)? Or does the actual import table structure differ?

> **Q6:** If the NID scan returns false positives, what is the safest way to validate `got_ptr` before dereferencing? Is `sys_mmapper_get_page_attribute` available from user-space SPRX context to check if an address is mapped and readable?

### 4. `hook_build_ba` Range Limitation for Branch-Back

**Location:** `hook.h` lines 188-196

```c
static inline uint32_t hook_build_ba(void *target)
{
    return 0x48000002 | (addr & 0x03FFFFFC);
}
```

The `ba` instruction encodes a 24-bit signed LI field, giving a reachable target range of ±32MB from the instruction address. But the encoding just ORs `addr & 0x03FFFFFC`, which only copies bits 2-25 of the address. For target addresses above `0x01FFFFFC`, this truncates the upper bits.

The trampoline branch-back targets `target+16` (the instruction after the 4-instruction patch). The trampoline itself may be far from the target game function (e.g., trampoline in SPRX .bss at ~0x30000000, target in game .text at ~0x00100000). The `ba` from trampoline back to `target+16` might not have enough range if the SPRX is loaded far from the game's .text.

**Questions for Expert:**

> **Q7:** Is the `hook_build_ba` implementation correct? The PowerPC `ba` instruction's LI field should be `(address >> 2) & 0x03FFFFFF` masked to 24 bits, with the opcode and flags filling the remaining bits. Current formula `addr & 0x03FFFFFC` only copies bits 2-25 — is this missing a right-shift?

> **Q8:** What is the typical load address range for SPRX modules injected via PS3MAPI relative to the game's .text segment? Can we rely on the trampoline being within ±32MB of all target game functions for the `ba` branch-back?

### 5. `get_game_toc` Backchain Walk Reliability

**Location:** `usb_hooks.c` lines 118-137

```c
static inline uint32_t get_game_toc(void)
{
    uint32_t caller_r1;
    uint32_t game_toc;
    __asm__ volatile ("lwz %0, 0(%%r1)\n\t" : "=r"(caller_r1));
    __asm__ volatile ("lwz %0, %1(%2)\n\t" : "=r"(game_toc)
        : "i"(HOOK_TOC_SAVE_OFFSET), "r"(caller_r1));
    return game_toc;
}
```

This reads r1's backchain to find the trampoline's stack frame, then reads offset 0x28 from it. However:

- The `-mprx` compiler prologue may push additional frames or save r2 differently before the `get_game_toc()` inline asm executes
- The compiler may reorder inline asm blocks with respect to each other
- The `__asm__ volatile` blocks are separate, so the compiler could insert code between them

**Questions for Expert:**

> **Q9:** With the Sony SDK `-mprx` ABI, does the `bl` from the trampoline to the C function guarantee that when the C function body starts (before any local variable allocation), r1 points to the trampoline's stack frame? Does the `-mprx` prologue push additional frames that would change the backchain depth?

> **Q10:** Is it safe to split the backchain read and the r2 read into two separate `__asm__ volatile` blocks, or should they be combined into a single asm block? What could go wrong with the split approach?

### 6. Inline Asm `mr` TOC Switching for Passthrough

**Location:** `usb_hooks.c` lines 318-323 and similar

```c
__asm__ volatile ("mr %0, %%r2" : "=r"(prx_toc));
game_toc = get_game_toc();
__asm__ volatile ("mr %%r2, %0" :: "r"(game_toc));
ret = ((tramp_fn_t)(uintptr_t)&g_usb_hooks.tramp_open_pipe)(...);
__asm__ volatile ("mr %%r2, %0" :: "r"(prx_toc));
```

The compiler assumes r2 is constant within a function (or at least within a basic block). Changing r2 via inline asm between C statements violates the ABI. The compiler may have already loaded values from the PRX's GOT and cached them in registers. Restoring the game's TOC and calling a function pointer could cause the called function to use the wrong GOT for its own data access.

**Questions for Expert:**

> **Q11:** Is the inline `mr` TOC switching safe? When we change r2 to the game's TOC and call a function pointer (`tramp_open_pipe`), does the called function (the original game code at the trampoline's saved instructions) rely on r2 being the game's TOC, or are the saved instructions self-contained and not TOC-relative?

> **Q12:** Could the compiler schedule a GOT-relative load (e.g., for a printf format string or a global variable address) between our `mr %%r2, %0` and the function call, using the wrong r2 and crashing? If so, should the TOC switch and function call be wrapped in a single asm block to prevent any compiler interference?

### 7. `hook.c` Wrong Opcode for `ppu_dcbst` Intrinsic

**Location:** `hook.c` lines 33-38

```c
static inline void ppu_dcbst(void *addr)
{
    __asm__ volatile (
        "dcbst 0, %0\n\t"
        "sync\n\t"
        "icbi 0, %0\n\t"
        "isync\n\t"
        ...
    );
}
```

The `dcbst` instruction invalidates a data cache block. But `dcbst` uses register RA|0 = 0 (r0 with value 0) as base, and RB = addr register. The syntax `"dcbst 0, %0"` means RA=0, RB=addr. This is correct for `dcbst` which computes effective address as `(RA|0) + RB` — so `0 + addr` = addr. Correct.

But the `icbi` instruction RIGHT AFTER the `dcbst` + `sync` is also odd — `icbi` should only be needed after the data cache sync and separate from it. The sequence should ideally be: `dcbst` all lines -> `sync` -> `icbi` all lines -> `isync`. By combining them in one asm block per cache line, the sync/icbi/isync sequence happens for EACH cache line, which is inefficient but should be correct.

**Questions for Expert:**

> **Q13:** Is the cache control asm sequence correct for the PPU? Specifically, should `dcbst` use RA=0 with RB=addr, or should it use RA=0 and RB=addr with a different syntax? Is each `dcbst` followed by sync+icbi correct, or should all `dcbst` operations for the full range be done first, then a single `sync`, then all `icbi`, then a single `isync`?

## Summary Table

| # | Suspect | Crash Type | Likelihood |
|---|---------|-----------|------------|
| 1 | Direct write to R-X .text (no `sys_process_write_memory`) | DSI | **VERY HIGH** |
| 2 | Trampoline in non-executable .bss/.data | ISI | **HIGH** |
| 3 | NID scanner deref of false-positive `got_ptr` | DSI | **MEDIUM** |
| 4 | `hook_build_ba` truncation / range issue | Instruction fetch error | **MEDIUM** |
| 5 | `get_game_toc` backchain depth wrong | Wrong TOC -> crash | **LOW** |
| 6 | Compiler reordering around TOC switch asm | Wrong TOC -> crash | **LOW-MEDIUM** |
| 7 | Cache control sequence | Subtle (would cause stale icache, not crash) | **LOW** |

## Next Step Request

Please confirm which of these is the actual crash cause so we can fix with surgical precision. If multiple suspects apply, prioritize by likelihood above.
