# Expert Consultation — 2026-07-28: libusbd FOUND — Final Blocker (HANDOFF v5)

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q [Full]
**Game:** LEGO Dimensions BLUS31473
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2 -fno-builtin -nodefaultlibs`
**SPRX:** ldtoypad.sprx (24,080 bytes v12), signed, PS3MAPI MODULE LOAD
**Status:** 🟢 GOT PATCHING WORKS (8/10 entries) | 🟡 Game doesn't call cellUsbdGetDeviceDescriptor

---

## BREAKTHROUGH: libusbd Found

After 11 iterations and ~3GB of memory scanning, the `cellFsWrite` safe probe
successfully located libusbd.sprx:

```
[USB] Scan FULL 0x02000000-0xC0000000 (retry 5s, max 100s)...
[USB] probing 0x02400000
[USB] libusbd base=0x02680000
```

**Key findings:**
- libusbd loads at **0x02680000** in the game heap (0x02000000-0x20000000)
- It is NOT loaded at "press start" — only after USB init (ToyPad screen)
- v10 retry loop caught it on first scan at the right time
- The `cellFsWrite` probe is conclusively proven to work (readback empty was buffering issue, not probe failure)

## All Targets Computed

```
TARGET_OPENPIPE   = 0x02680244  (base + 0x244)
TARGET_TRANSFER   = 0x026804B4  (base + 0x4B4)
TARGET_CLOSEPIPE  = 0x02680380  (base + 0x380)
TARGET_GETDEVDESC = 0x0268061C  (base + 0x61C)
TARGET_CTRLXFER   = 0x026807C8  (base + 0x7C8)
```

Trampolines ready at `0x11720000` (64KB R-W-X page, 5 x 64-byte trampolines).

## The Final Blocker: Cannot Write to libusbd's .text

Three methods tried, all failed:

| Method | Result |
|--------|--------|
| **PS3MAPI MEMORY SET** (HTTP) | Returns 226 "OK" but verification shows original data unchanged |
| **Raw pointer write from SPRX** | DSI exception — SPRX thread killed (log stops mid-write) |
| **PS3MAPI MEMORY SET** (short PID) | Returns 451 "Local error" |

**The MMU write-protects loaded module .text sections** even on Cobra 8.5 CFW.
The SPRX runs inside the game process but still cannot write to libusbd's code
pages. PS3MAPI's `poke_process` returns success but the write is silently
dropped by the hypervisor.

## What We've Proven Works

- ✅ cellFsWrite safe probe: scans 0x02000000-0xC0000000 without crashing
- ✅ Module name detection: "cellUsbd_Library" at base+0x94EC is correct
- ✅ Retry loop: handles lazy module loading (libusbd appears at USB init)
- ✅ Trampoline generation: 5 hooks at R-W-X page 0x11720000
- ✅ IPC file: STATUS/TARGET_*/TRAMP_*/HEARTBEAT format proven
- ✅ Preamble builder: PowerPC lis/ori/mtctr/bctr sequence ready
- ✅ Game stability: zero crashes from scanning 3GB of address space

## Targeted Questions

**Q1: How to make libusbd's .text writable on Cobra 8.5?**
Is there an LV2 syscall to change page permissions from PRX context?
(`sys_mmapper_allocate_address`? `sys_memory_set_protection`?) Or a
Cobra-specific syscall for modifying module page tables?

**Q2: Is there a Cobra LV2 POKE endpoint we can call from C?**
PS3MAPI's HTTP MEMORY SET doesn't write to .text. But does Cobra expose
an LV2-level poke function that the SPRX can call directly (bypassing
the HTTP layer and its limitations)?

**Q3: Can we hook through the game's PLT/GOT instead?**
Instead of modifying libusbd's .text (read-only), can we modify the
game EBOOT's import table? The game's PLT stubs resolve through GOT
entries in EBOOT .data (writable). If we know the GOT addresses for
cellUsbd imports, can we redirect them to our trampolines?

**Q4: Does Evilnat 4.93 have a debug/developer mode that relaxes MMU?**
Some CFW versions allow disabling write protection for module .text
via a configuration flag or system call.

**Q5: Can we use `sys_prx_load_module` to load a patched libusbd.sprx?**
If we can't modify the running libusbd, can we load our own version
with pre-patched function entries? The game would then resolve imports
to our patched module instead of the firmware's.

## Build

```
SDK: DUPLEX 3.40
ppu-lv2-gcc -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs
Link: -llv2_stub -lfs_stub -lnet_stub -lusbd_stub -lsysmodule_stub
Sign: oscetool 0.9.2 -5 APP (WSL)
SPRX: 23,472 bytes (v11 — remove self-write for v12)
```

---

## APPENDIX A: GOT/PLT Patching Approach (v12 — UNTESTED)

**Date:** 2026-07-28 17:00 (appended after building v12 but before PS3 test)
**Status:** 🟡 Built, deployed, awaiting PS3 availability for injection test

### Theory

Since libusbd's `.text` is MMU write-protected (Q1-Q2 unresolved), we pivot
to Q3: **hijack the game EBOOT's PLT/GOT entries** instead.

On PowerPC CellOS, the game's PLT stubs dereference GOT entries in the
EBOOT's writable `.data` segment. Each GOT entry for a `cellUsbd*` import
holds a 32-bit pointer to the **libusbd OPD** (Official Procedure
Descriptor — a 12-byte struct of `{code_addr, toc_addr, env_ptr}`).

The insight: if we can find the GOT entries that point to libusbd OPDs,
we can overwrite them with pointers to **our own OPDs** (placed in the
already-allocated R-W-X trampoline page). The game's PLT stubs then
resolve through our OPDs instead, calling our trampolines → C hooks.

### Why We're Confident This Can Work

1. **EBOOT `.data` is writable** — unlike libusbd `.text`, the game's
   own data segment is R-W. SPRX code can write to it via raw pointer.

2. **We know the exact OPD pointer values** — `cellUsbdOpenPipe`'s OPD
   is at `0x02680244`, `cellUsbdInterruptTransfer` at `0x026804B4`, etc.
   Five unique 32-bit values to scan for.

3. **False positive rate is near zero** — scanning ~30MB of EBOOT data
   (0x00010000–0x02000000) for five specific 32-bit magic numbers yields
   an expected ~0.002 false positives. Finding all five values clustered
   in close proximity (the GOT table) confirms their identity.

4. **No crash from scan** — raw 32-bit reads across EBOOT `.data` are
   safe; we're reading mapped, readable memory.

### v12 Implementation

```
Trampoline page layout at 0x11720000 (64KB, R-W-X):

Offset  Size   Content
──────────────────────────────
0       64     OpenPipe trampoline (PPC asm → my_cellUsbdOpenPipe)
64      64     InterruptTransfer trampoline
128     64     ClosePipe trampoline
192     64     GetDeviceDescriptor trampoline
256     64     ControlTransfer trampoline
320     4      Heartbeat counter (volatile uint32_t)
324     188    [unused padding]
512     12     OPD for OpenPipe       {tramp+0,   SPRX_TOC, 0}
524     12     OPD for Transfer       {tramp+64,  SPRX_TOC, 0}
536     12     OPD for ClosePipe      {tramp+128, SPRX_TOC, 0}
548     12     OPD for GetDevDesc     {tramp+192, SPRX_TOC, 0}
560     12     OPD for ControlXfer    {tramp+256, SPRX_TOC, 0}
```

**Algorithm** (`usb_hook_init()`, Step 4.5):

1. Extract SPRX TOC from `my_cellUsbdOpenPipe`'s OPD (word [1] of the
   function pointer — each C function is an OPD pointer).
2. Create 5 synthetic OPDs at trampoline page offset 512, each pointing
   `code_addr` → its trampoline, `toc_addr` → SPRX TOC, `env_ptr` → 0.
3. For each of the 5 known libusbd OPD addresses (the "needles"):
   - Scan `0x00010000` through `0x02000000` (EBoot `.data`) as `uint32_t*`
   - On match, overwrite the GOT entry with the address of our synthetic OPD
   - Stop after 2 matches per function (GOT primary + PLT resolver backup)
4. Log results to papertrail: `[USB] GOT OpenPipe: 2 entries patched` etc.

### Known Risks / Open Questions

| Risk | Mitigation |
|------|-----------|
| **GOT entries not in 0x00010000–0x02000000** | The EBOOT's `.data`/`.sdata`/`.got` are typically in this range. If the scan finds nothing, we fall back gracefully with a papertrail message. |
| **Lazy binding interference** | PS3 PLT stubs are typically eager-resolved (full OPD pointers in GOT at module load time). If the game uses lazy binding, GOT entries may contain PLT resolver addresses instead of libusbd OPDs, and our needle match would fail. |
| **GOT entries write-protected after relocation** | Some CFW/ELF loaders may mark `.got` read-only after initial relocation. If true, writes would DSI-crash like libusbd `.text`. The scan itself is read-only and safe. |
| **SPRX TOC mismatch** | We use our SPRX's TOC in the synthetic OPDs. When the game calls through our OPD, r2 is loaded with the SPRX TOC. The trampoline saves the game's TOC (in r2 at call time) to the stack, loads SPRX TOC, calls the C hook, then restores the game's TOC before returning. This is proven correct in the trampoline generator. |
| **Other modules also have GOT entries** | If `libusbd.sprx` itself has GOT entries referencing its own OPDs (self-calls), our scan might pick those up too. We cap at 2 matches per needle to limit collateral damage. |

### Build Info (v12)

```
SPRX: 23,792 bytes signed (144,740 bytes unsigned)
Commit: not yet committed (pending test results)
Key changes:
  - Removed ~40 lines of self-write preamble code (v11 crash)
  - Added ~70 lines of GOT scanning + synthetic OPD creation
  - Added dcbst/sync/icbi/isync cache flush per expert review
    (Critical: without this, PPU may read stale GOT from icache)
```

---

## APPENDIX B: Expert Response — 2026-07-28 18:47

**Verdict:** v12 GOT patching is **validated as the canonical approach** for
late-injection hooking on CellOS. The expert confirms:

- `.text` pages of system PRX modules are permanently R-X; no userland
  bypass exists (not even Cobra syscall 35 `sys_lv2_poke` is safe).
- GOT pointer scanning in EBOOT `.data` is the Holy Grail — same method
  used by professional GTA V SPRX modders.
- Lazy binding is already resolved at T+60s (game has initialized USB),
  so GOT entries contain the final libusbd OPD pointers.
- The synthetic OPD `{tramp_addr, SPRX_TOC, 0}` is ABI-compliant.

**One critical addition applied: PowerPC cache flush.**

After each GOT overwrite:
```c
__asm__ volatile (
    "dcbst 0, %0\n\t"   // flush data cache line to RAM
    "sync\n\t"          // ensure store completes
    "icbi 0, %0\n\t"    // invalidate instruction cache line
    "isync"             // flush instruction pipeline
    :: "r"(scan) : "memory"
);
```

Without this, the PPU's instruction fetch may use a stale cached GOT
pointer, and the PLT stub would still call the original libusbd OPD.

---

## APPENDIX C: v12 Test Results — GOT Patching Works, Game Doesn't Call GetDeviceDescriptor

**Date:** 2026-07-28 22:15 (tested on PS3, LEGO Dimensions at "Connect ToyPad" screen)

### GOT Patching: SUCCESS

```
[USB] libusbd base=0x02680000
[USB] OPDs at 0x11720200, TOC=0x028F82B0
[USB] GOT OpenPipe: 2 ent
[USB] GOT Transfer: 2 ent
[USB] GOT ClosePipe: 2 ent
[USB] GOT GetDevDesc: 1 ent
[USB] GOT CtrlXfer: 1 ent
[USB] GOT done: 8 entries. LIVE.
[USB] IPC written with real TARGET_* addresses
=== Entering main loop ===
```

- 8 of 10 possible GOT entries patched across all 5 functions
- SPRX did NOT crash (cellFsWrite page probing prevented DSI)
- IPC file written and verified
- Game remained stable, no freeze

### Trojan Horse: DID NOT FIRE

- USB flash drive plugged in twice — no "TROJAN HORSE FIRED" in boot log
- Entering XMB overlay and returning to game (triggers USB re-scan) — still no hook invocation
- The game **never calls `cellUsbdGetDeviceDescriptor`** during USB enumeration

### Key Finding

The game's EBOOT imports `cellUsbdGetDeviceDescriptor` (we found and patched
1 GOT entry), but the "Connect ToyPad" screen's USB scan doesn't use it. The
game must enumerate USB devices through a **different API** — possibly:

- `cellUsbdGetDeviceList` — bulk device enumeration
- `cellUsbdGetDeviceDescriptor2` — alternative descriptor function
- Direct `cellUsbdControlTransfer` with GET_DESCRIPTOR requests
- A Sony-specific LDD callback that receives device info from the kernel

### New Question for Expert

**Q6: What USB enumeration API does LEGO Dimensions (TT Games engine) use?**

We've GOT-patched `cellUsbdGetDeviceDescriptor` but the game never calls it.
The game reaches the "Connect ToyPad" screen (USB is initialized) but its
device discovery doesn't route through this function.

Options we need guidance on:
1. Is there a different `cellUsbd*` function that serves as the primary
   entry point for game USB device discovery (e.g., `cellUsbdGetDeviceList`)?
2. Does the CellOS LDD framework deliver device descriptors to the game's
   probe callback without the game calling `cellUsbdGetDeviceDescriptor`?
3. Should we also GOT-patch `cellUsbdControlTransfer` to intercept
   GET_DESCRIPTOR USB requests (the game might read descriptors directly
   via control transfers rather than the convenience wrapper)?

### Build Info (v12)

```
SPRX: 24,080 bytes signed (145,916 bytes unsigned)
Architecture: GOT pointer scanning with cellFsWrite page probing
Cache flush: dcbst/sync/icbi/isync on each GOT write
Scan range: 0x00010000-0x20000000 (three 64KB-probed chunks)
```
