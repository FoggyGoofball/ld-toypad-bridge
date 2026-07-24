# Expert Questions — LD-ToyPad Bridge

**Date**: 2026-07-24
**Project**: Intercept LEGO Dimensions' cellUsbd calls to route Toy Pad traffic over UDP
**Status**: SPRX injects and loads correctly, but NID triplets CANNOT BE FOUND in game memory

---

## Build Environment

| Component | Detail |
|-----------|--------|
| **SDK** | Official Sony PS3 SDK v3.400 (SN Systems DUPLEX) |
| **Compiler** | `ppu-lv2-gcc.exe` (GCC-based, `-mprx` flag) |
| **Target** | SPRX (Sony PRX format), signed with OpenSCETool 0.9.2 |
| **Linker flags** | `-mprx -nodefaultlibs -llv2_stub -lfs_stub -lnet_stub -lusbd_stub` |
| **CFW** | REBUG 4.84.2 REX + COBRA 8.x |
| **PS3MAPI** | webMAN MOD 1.47.48c+ JSON RESTful API |
| **Injection** | Hot-injection into LEGO Dimensions game process via `MODULE LOAD / UNLOAD` endpoints |
| **Build host** | Windows 11, GCC cross-compiler, WSL for oscetool signing |

## Project Architecture (Refactored to Direct GOT Overwrite)

We inject a `.sprx` into the LEGO Dimensions game process via PS3MAPI. The SPRX:

1. **Allocates** a 64KB R-W-X page via `sys_memory_allocate()` at ~`0x10320000`
2. **Generates** 4 dynamic PowerPC trampolines (64 bytes each) in that page
3. **Scans** game memory for 12-byte NID triplets `{ NID(4), reserved(4), GOT_ptr(4) }` to find the game's cellUsbd GOT entries
4. **Overwrites** the resolved GOT slots (containing real `libusbd.sprx` addresses) with our trampoline addresses
5. **Result**: Game calls cellUsbd → PLT stub loads trampoline addr from GOT → our C hook runs

![Hook Flow](game → PLT stub → lwz from GOT → mtctr → bctr → trampoline → C hook)

## Core Problem: NID Triplets NOT FOUND

**Both the in-SPRX scanner AND the PC-side `scan-game-nids.js` fail to find the NID triplets.**

### What the in-SPRX scanner does (`scan_nid_got_slot()` in `usb_hooks.c`):

```c
static const scan_region_t g_scan_regions[NUM_SCAN_REGIONS] = {
    { 0x00100000, 0x00900000, "game .text region 1"   },
    { 0x00900000, 0x01000000, "game .text gap"         },
    { 0x01000000, 0x02000000, "game .text region 2"   },
    { 0x02000000, 0x03000000, "game .text region 3"   },
    { 0x03000000, 0x05000000, "game .text region 4"   },
};

// Scanning logic (simplified):
for each region:
    uint32_t *words = (uint32_t*)region_start;
    for i = 0 to nwords-3 step 3:
        nid      = words[i+0]  // native read (big-endian on PS3)
        reserved = words[i+1]
        got_ptr  = words[i+2]
        if (nid == target_nid && reserved < 0x1000 && got_ptr is valid) {
            *out_got_slot_ptr = got_ptr;
            return 0;  // FOUND
        }
```

### What the PC-side scanner does (`scan-game-nids.js`):

```javascript
const SCAN_REGIONS = [
    { start: 0x00100000, size: 0x00800000, desc: 'game .text region 1' },
    { start: 0x01000000, size: 0x01000000, desc: 'game .text region 2' },
    { start: 0x02000000, size: 0x01000000, desc: 'game .text region 3' },
    { start: 0x30000000, size: 0x00800000, desc: 'cellUsbd PRX' },
    { start: 0x40000000, size: 0x01000000, desc: 'cellUsbd PRX alt' },
];

// Tries both LE and BE:
for each region (via PS3MAPI getmem):
    for each NID:
        // Try little-endian
        nidLE = Buffer.alloc(4); nidLE.writeUInt32LE(nid)
        if buf.indexOf(nidLE) >= 0 → FOUND
        
        // Try big-endian  
        nidBE = Buffer.alloc(4); nidBE.writeUInt32BE(nid)
        if buf.indexOf(nidBE) >= 0 → FOUND
```

### NIDs we're searching for:

```c
#define NID_CELL_USBD_INIT          0x7F5F00D3
#define NID_CELL_USBD_OPENPIPE      0x1AB6D80B
#define NID_CELL_USBD_TRANSFER      0x7B4436CE
#define NID_CELL_USBD_CLOSEPIPE     0x2F82F1A5
```

### What the PC scanner output shows:

```
=== Scanning PS3 game memory for cellUsbd NIDs ===
PS3 IP: 192.168.0.47
Game PID: 0x1010200 (16843264)

Trying NIDs (32-bit):
  0x7F5F00D3 (cellUsbdInit)
  0x1AB6D80B (cellUsbdOpenPipe)
  0x7B4436CE (cellUsbdTransfer)
  0x2F82F1A5 (cellUsbdClosePipe)

  - game .text region 1 (0x100000-0x900000): no NIDs found
  - game .text region 2 (0x1000000-0x2000000): no NIDs found
  - game .text region 3 (0x2000000-0x3000000): no NIDs found
  - cellUsbd PRX (0x30000000-0x30800000): no NIDs found
  - cellUsbd PRX alt (0x40000000-0x41000000): no NIDs found

✗ cellUsbdInit: NOT FOUND
✗ cellUsbdOpenPipe: NOT FOUND
✗ cellUsbdTransfer: NOT FOUND
✗ cellUsbdClosePipe: NOT FOUND
```

### Confirmatory: The IPC file the SPRX writes (TARGET_* = 0x0):

```
STATUS=ready
TRAMP_BASE=0x10320000
TRAMP_INIT=0x10320000
TRAMP_OPENPIPE=0x10320040
TRAMP_TRANSFER=0x10320080
TRAMP_CLOSEPIPE=0x103200C0
TARGET_INIT=0x0
TARGET_OPENPIPE=0x0
TARGET_TRANSFER=0x0
TARGET_CLOSEPIPE=0x0
```

So the SPRX writes all TARGET_* as 0x0 — confirming the NID scan found nothing.

---

## QUESTIONS

### Q1: Are the NID values correct for LEGO Dimensions (BLUS31548 / BLES02206)?

We extracted our NID values from prior work analyzing the game's `.self` file:

```c
#define NID_CELL_USBD_INIT          0x7F5F00D3
#define NID_CELL_USBD_OPENPIPE      0x1AB6D80B
#define NID_CELL_USBD_TRANSFER      0x7B4436CE
#define NID_CELL_USBD_CLOSEPIPE     0x2F82F1A5
```

**These came from a PC-side analysis tool, not from officially documented SDK headers.** The official Sony SDK would define `cellUsbd` NID values in its headers (e.g., `<cell/cell_usbd.h>` or similar).

**Question: Are these NID values correct? Can you confirm the official NID values for cellUsbdInit, cellUsbdOpenPipe, cellUsbdInterruptTransfer, and cellUsbdClosePipe from the Sony SDK?**

### Q2: What is the true in-memory format of import stub tables on Sony SDK -mprx modules?

We assumed the import stub table uses this 12-byte triplet format:

```
Offset 0: NID        (uint32_t) — function identifier hash
Offset 4: reserved   (uint32_t) — should be 0
Offset 8: GOT_ptr    (uint32_t) — pointer to GOT slot
```

**This was a guess based on community knowledge, not SDK documentation.**

**Question: What is the actual in-memory format of a PRX import stub entry as laid out by the Sony SDK's PRX loader? Is our 12-byte `{ NID, reserved, GOT_ptr }` correct, or does the structure have different fields/sizes? Do COBRA CFW PRX loaders differ from Sony's official loader?**

### Q3: Where in memory do PRX import stubs live for a game process?

We search `0x00100000` through `0x05000000` (game memory) AND `0x30000000-0x30800000` (libusbd.sprx PRX region). The NIDs are not found in any of these regions.

**Possible explanations:**
a) The NIDs are in a memory region we're not scanning (e.g., `0x05000000-0x10000000`, or somewhere else entirely)
b) The import stubs are stored differently than expected (see Q2)
c) The game resolves cellUsbd manually (not via standard PRX import stubs)

**Question: For a PS3 game that imports from libusbd.sprx, where would the import stub table normally be located in memory? Is it in the game's `.text`, `.rodata`, or `.data` segment? Could it be in a dynamically-allocated region?**

### Q4: Can we use `sys_prx_get_module_id_by_name` from within an SPRX to resolve other PRX modules?

Alternative to NID scanning: Instead of searching for NID triplets, could our SPRX directly resolve the cellUsbd module's function addresses using the PRX module system?

```c
// Hypothetical approach:
sys_prx_module_info_t modinfo;
sys_prx_get_module_id_by_name("libusbd", &mod_id, 0, 0);
sys_prx_get_module_info(mod_id, 0, &modinfo);
// Find exported function addresses from modinfo.segments
```

**Question: Can an SPRX loaded into a game process call `sys_prx_get_module_id_by_name()` to find another loaded PRX (like libusbd.sprx)? Are there `sys_prx_*` stubs available for SDK -mprx builds? Which library provides them (`-llv2_stub` maybe not enough — do we need `-lprx_stub`)?**

### Q5: Direct GOT overwrite — safety and correctness concerns

Our refactored approach overwrites the game's resolved GOT slots (containing real `libusbd.sprx` function addresses like `0x30XXXXXX`) with our trampoline addresses (`~0x10320000`):

```c
volatile uint32_t *got_slot = (uint32_t*)got_slot_ptr;
*got_slot = trampoline_addr;  // Overwrite resolved address with our tramp

// Cache flush
__asm__ volatile (
    "dcbst 0, %0\n\tsync\n\ticbi 0, %0\n\tisync"
    :: "r"(got_slot) : "memory"
);
```

The concern: If the game calls `cellUsbdInit` BEFORE we inject (which it does — injection happens ~60s after boot), the GOT slot is resolved to the real address. But the game may use a **copy-on-write or lazy-binding scheme** that re-reads the original NID triplet and re-resolves the import, overwriting our trampoline address.

**Question: Does the PowerPC PRX loader on COBRA CFW ever re-resolve GOT slots after initial resolution? Is a GOT overwrite safe from being reverted by the lazy-binding mechanism? Is there any case where the PRX loader would write back the original resolved address over our trampoline?**

### Q6: Could LEGO Dimensions resolve cellUsbd manually (not through standard import stubs)?

Some PS3 games call `sys_prx_load_module` + `sys_prx_get_module_id_by_name` + manual symbol lookup instead of relying on the standard PRX import resolution. If LEGO Dimensions does this for cellUsbd, there would be NO standard NID triplets or PLT stubs to find — the resolved function pointers would be stored in arbitrary game data structures.

**Question: How can we verify whether LEGO Dimensions uses standard PRX imports vs. manual resolution for cellUsbd? The game's `libusbd.sprx` file exists on disk — would dumping the game's `.self` file's `.plt` section offline show the expected stubs? Could we use PS3MAPI to dump the game's import table from the running process?**

### Q7: Dynamic trampoline — is the PowerPC instruction encoding correct?

Our trampoline generator (`trampoline_gen.c`) produces 64-byte trampolines in R-W-X memory at runtime:

```c
void create_hook_trampoline(uint32_t *tramp, void *c_func, int toc_arg_reg)
{
    const ppc_opd_t *opd = (const ppc_opd_t *)c_func;
    uint32_t code = opd->code_addr;  // C hook's .text address
    uint32_t toc  = opd->toc_addr;   // C hook's SPRX TOC
    
    tramp[i++] = 0x9421FFA0u;        // [0]  stwu r1, -0x60(r1)
    tramp[i++] = 0x7C0802A6u;        // [1]  mflr r0
    tramp[i++] = 0x90010064u;        // [2]  stw r0, 0x64(r1)
    tramp[i++] = 0x90410028u;        // [3]  stw r2, 0x28(r1)  -- save game TOC
    
    tramp[i++] = 0x7C401378u | ...;  // [4]  mr toc_arg_reg, r2
    
    tramp[i++] = 0x3C400000u | ...;  // [5]  lis r2, toc@h
    tramp[i++] = 0x60420000u | ...;  // [6]  ori r2, r2, toc@l
    
    tramp[i++] = 0x3D800000u | ...;  // [7]  lis r12, code@h
    tramp[i++] = 0x618C0000u | ...;  // [8]  ori r12, r12, code@l
    
    tramp[i++] = 0x7D8903A6u;        // [9]  mtctr r12
    tramp[i++] = 0x4E800421u;        // [10] bctrl
    
    tramp[i++] = 0x80410028u;        // [11] lwz r2, 0x28(r1)   -- restore game TOC
    tramp[i++] = 0x80010064u;        // [12] lwz r0, 0x64(r1)
    tramp[i++] = 0x7C0803A6u;        // [13] mtlr r0
    tramp[i++] = 0x38210060u;        // [14] addi r1, r1, 0x60
    tramp[i++] = 0x4E800020u;        // [15] blr
}
```

**Question: Is this PowerPC trampoline ABI-correct for the PS3? Specifically:**
- **Does `cellUsbdInit`, `cellUsbdOpenPipe`, etc. expect arguments in r3-r10 (standard calling convention)?**
- **Are we handling the TOC (r2) correctly?**
- **Should we save/restore r12 (we don't currently — is that a problem)?**
- **Is 64 bytes (16 instructions) sufficient for the full prologue/epilogue?**
- **Does `bctrl` (not `blrl`) correctly link the return address for the C hook?**

### Q8: SYS_MEMORY_ALLOCATE for R-W-X memory — is this the correct syscall?

We use:

```c
// From ldd_driver.c:
int sys_memory_allocate(uint32_t size, uint32_t flags, uint32_t *addr);
```

Our trampoline page allocation:
```c
uint32_t tramp_page = 0;
ret = sys_memory_allocate(0x10000, 0x4000000F, &tramp_page);
// flags: 0x4000000F = SYS_MEMORY_PAGE_RWX (from syscall.h)
```

This succeeds (returns tramp_page = ~0x10320000).

**Question: Is `sys_memory_allocate()` the correct/only way to get R-W-X memory within a game process on COBRA CFW? Are there restrictions on which flags combinations are allowed? Could there be a different/simpler approach using `mmap` or malloc-like functions available to an SPRX?**

### Q9: Debugging — can we dump the game's actual import stubs from a .self file?

The most reliable way to verify our assumptions would be to examine the LEGO Dimensions `.self` / `.elf` file offline:

- Extract the game's ELF binary (from the installed game on HDD or from a PKG)
- Dump the `.plt` section to see the actual PLT stub patterns
- Dump the `.rela.plt` relocation entries to see expected GOT values
- Dump the `.rodata` section to find the actual NID import table

**Question: What's the best way to get the LEGO Dimensions ELF for offline analysis? Can we dump the currently-running game's segments via PS3MAPI getmem? Is there a way to extract the game's `.self` file from the HDD and decrypt it with existing tools (RPCS3 keys, make_self_npdrm, etc.)?**

### Q10: Module injection timing — GOT already resolved at injection time

We inject the SPRX **60 seconds after game start** to let the game stabilize. By this time:
- The game has already called `cellUsbdInit()` (at minimum, likely during USB subsystem init at boot)
- Possibly also `cellUsbdOpenPipe()` and `cellUsbdInterruptTransfer()` during initial USB device enumeration
- All 4 GOT slots are resolved to real `libusbd.sprx` addresses

The injection timing (60s) was chosen to avoid corrupting the game's boot process.

**Question: Is there a known window after game start where cellUsbd imports are NOT yet resolved? If we inject IMMEDIATELY when the game PID first appears (instead of waiting 60s), would the GOT slots still be unresolved (pointing to PLT stubs)? Or does the PRX loader resolve all GOT slots at module load time (before `main()` runs)?**

---

## Appendix A: Full File Map

| File | Purpose | Key Function |
|------|---------|-------------|
| `sprx-plugin/usb_hooks.c` | NID scan, GOT overwrite, IPC | `scan_nid_got_slot()`, `got_overwrite_one()`, `usb_hook_init()` |
| `sprx-plugin/trampoline_gen.c` | Dynamic PowerPC trampoline | `create_hook_trampoline()` |
| `sprx-plugin/main.c` | Entry point, worker thread | `module_start()`, `worker_thread()` |
| `sprx-plugin/network.c` | UDP transport | `network_init()`, `network_send_data()` |
| `sprx-plugin/toypad_state.c` | Toy Pad state machine | `toypad_state_init()` |
| `sprx-plugin/debug.c` | Paper-trail logging | `debug_printf()`, `debug_write_progress()` |
| `ld-toypad-server/scripts/inject-sprx.js` | Node.js injector | `injectSprx()`, `waitForIpcAndVerify()` |
| `ld-toypad-server/scripts/scan-game-nids.js` | PC-side NID scanner | `scanMemory()` |
| `ld-toypad-server/server.js` | Bridge server | UDP relay to web UI |

## Appendix B: Execution Trace (Injector Log)

```
Step 1: Verifying PS3MAPI connectivity... ✓
Step 2: Detecting game process... PID=0x1030200
Step 3: Waiting 30s for game to initialize...
Step 4: Injecting SPRX... ✓ (Two-pass load)
Step 5: Polling SPRX init progress... ✓ g_init_progress address: 0xd90514
    [progress] SPRX at step 20 (worker thread entered)
    [progress] SPRX at step 70 (usb_hook_init() returned)
    ✓ IPC file detected

IPC parsed:
  TRAMP_BASE=0x10320000
  TRAMP_INIT=0x10320000      TRAMP_OPENPIPE=0x10320040
  TRAMP_TRANSFER=0x10320080  TRAMP_CLOSEPIPE=0x103200C0
  TARGET_INIT=0x0            TARGET_OPENPIPE=0x0
  TARGET_TRANSFER=0x0        TARGET_CLOSEPIPE=0x0

⚠ All TARGET_* = 0x0 — GOT overwrites FAILED (NIDs not found)
```

## Appendix C: Debugging History

### Previous Approaches (all abandoned):

1. **Static Assembly Trampoline** (`toc_trampoline.s`): Wrote PowerPC assembly files that had to be rebuilt for each TOC/code change. Abandoned for dynamic generation.

2. **PS3MAPI Preamble Injection**: SPRX scanned for NIDs → got PLT stub addresses → wrote GOT addresses to IPC file → Node.js injector wrote 4-instruction preamble into game .text via PS3MAPI MEMORY SET. Abandoned because PS3MAPI required Ring 0 and was fragile.

3. **NID Triplet + GOT Read** (current code): SPRX scans for NID triplets, reads GOT slot value, decides where to hook. Problem: GOT is already resolved. NIDs themselves not found in current scan.

### Commands for reproducing:

```powershell
# Build SPRX
powershell -ExecutionPolicy Bypass -File "sprx-plugin/build-all.ps1"

# Deploy to PS3 via FTP
powershell -ExecutionPolicy Bypass -File "ftp-deploy.ps1"

# Run the injector (with game running)
node ld-toypad-server/scripts/inject-sprx.js --ps3-ip 192.168.0.47 --verbose
```

---

*Prepared by Cline for expert consultation, 2026-07-24*
