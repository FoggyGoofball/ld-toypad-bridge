# Expert PS3 Homebrew Developer Questions
## Date: 2026-07-21

---

## Context

We are building an SPRX that hooks cellUsbd functions in LEGO Dimensions (BLUS31473, game PID 0x1010200) on a PS3 with webMAN MOD 1.47.48c / Cobra CFW. The SPRX is injected into the game process via PS3MAPI `MODULE LOAD`, then uses `sys_memory_allocate` for executable trampoline pages, and communicates hook addresses to a Node.js orchestrator via HDD IPC files. The actual preamble (4 instructions: lis/ori/mtctr/bctr) is written by the Node.js process via PS3MAPI `MEMORY SET` (Ring 0, bypasses R-X .text protection).

**The problem:** The NID scanner inside the SPRX cannot find the game's cellUsbd import stubs. We've traced this to a chicken-and-egg problem with CellOS lazy binding.

---

## Question 1: CellOS Import Stub Format & NID Scanning

**Background:**
Our scanner searches for 12-byte triplets in game memory:
```
Offset  Size  Field
0       4     NID (e.g. 0x7F5F00D3 for cellUsbdInit)
4       4     Reserved (usually 0)
8       4     GOT pointer → dereference to get function address
```

We look for these NID values:
- `cellUsbdInit`:     0x7F5F00D3
- `cellUsbdOpenPipe`: 0x1AB6D80B
- `cellUsbdTransfer`: 0x7B4436CE
- `cellUsbdClosePipe`: 0x2F82F1A5

We scanned the game's mapped memory range 0x00100000-0x00900000 (8MB) from the PC side using `getmem.ps3mapi` and found **zero matches** for these 32-bit values in either big-endian or little-endian format.

**Question:**
1. Are these the correct 32-bit NID values for cellUsbd on firmware 4.89+ (our PS3 is on 4.89)? CellOS NIDs are derived from a hash of the function prototype — are we using the right base/hash algorithm?
2. Could LEGO Dimensions use **64-bit NIDs** (truncated SHA-1) instead of 32-bit? If so, how do we determine the 64-bit NID values?
3. Could the import stub format be different from our 12-byte triplet assumption? Is there a separate **IML (Import Module List)** structure we should be parsing instead?
4. What's the canonical way to enumerate a running PRX/game's imported functions on CellOS at runtime?

**Relevant code** (`sprx-plugin/usb_hooks.c` lines 392-437):
```c
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

        uint32_t *got_slot = (uint32_t*)(uintptr_t)got_ptr;
        uint32_t func_addr = *got_slot;

        // REJECT PLT STUBS (address < 0x30000000)
        if (func_addr == 0 ||
            func_addr < 0x30000000 ||
            func_addr > 0x4FFFFFFF) {
            continue;
        }
        *out_addr = (void*)(uintptr_t)func_addr;
        return 0;
    }
    return -1;
}
```

---

## Question 2: Chicken-and-Egg — PLT Stubs vs Resolved Functions

**Background:**
The SPRX NID scanner rejects GOT entries that point to PLT stubs (< 0x30000000 range). The retry loop tries for 20 seconds (10 × 2s), but the game never resolves these GOT entries because **it never calls cellUsbdInit** until a Toy Pad is actually detected — which requires our hooks to be installed first.

**Question:**
1. For a game on CFW that loads at boot with cellUsbd as a dependency, are the import stubs' GOT slots *ever* resolved before the first actual call to the function? Or do they always start as PLT stubs?
2. If they stay as PLT stubs until first call, what's the recommended strategy? Options we've considered:
   - **Option A:** Hook the PLT stub address directly (the 16-byte trampoline in `.plt`) instead of the resolved function. The PLT stub format on CellOS is: `lis r12, hi16(GOT_offset) / b dest / ...`. Can we replace this with our own preamble?
   - **Option B:** Parse the game ELF from filesystem to get the real function addresses directly from the import table, avoiding runtime GOT dereference entirely.
   - **Option C:** Trigger the game to resolve cellUsbd first by having the SPRX call cellUsbdInit itself (our SPRX links against -lusbd_stub), then capture the resolved address.
   - **Option D:** The 16-byte PLT stub itself is the "real" entry point we should hook, not the resolved function. What's the actual CellOS PLT stub format on PowerPC 32-bit?

3. Is there a `sys_prx_get_module_info` or similar API that can give us a function's address from a loaded PRX by NID, bypassing the game's GOT entirely? Something like `sys_prx_get_import_address(module_id, nid)`?

---

## Question 3: PS3MAPI Process Memory Maps

**Background:**
We can read process memory via `GET /getmem.ps3mapi?proc=0x1010200&addr=X&len=Y`. The game memory starts at 0x00100000. The PC-side scanner we wrote scans in 8KB chunks sequentially. This is slow over HTTP.

**Question:**
1. Does PS3MAPI have an endpoint to dump the process memory map (mappings, not contents)? Something like: `GET /ps3mapi.ps3?PROCESS%20GETMAPS%200x1010200`?
2. What are the typical memory mapping ranges for a game PRX on CFW 4.89? Specifically:
   - Where is the `.text` section of the main game ELF loaded?
   - Where is the `.got` section?
   - Where are the `.plt` stubs?
   - Where are imported PRX modules (cellUsbd, etc.) mapped?
3. If we dump `/dev_hdd0/game/BLUS31473/` via FTP, what's the typical ELF/SELF filename for the main game executable? Is it `USRDIR/EBOOT.BIN`? Or a `.sprx` in the game directory?

---

## Question 4: ELF Parsing — Finding Import Tables on Disk

**Background:**
Our planned fix is to move NID scanning to the Node.js injector. The injector will download the game's ELF from the PS3 filesystem via FTP, parse it, find the import stubs for cellUsbd functions, and calculate the target addresses (game base address + section offset + stub offset).

**Question:**
1. Can a game's ELF/SELF be directly read from the PS3 filesystem? Is it:
   - `/dev_dvd/PS3_GAME/USRDIR/EBOOT.BIN` (for disc-based)?
   - `/dev_hdd0/game/BLUS31473/USRDIR/EBOOT.BIN` (if installed)?
   - Does webMAN MOD serve these via HTTP?
2. Is the game's in-memory base address (its "game base") retrievable via PS3MAPI? Something like `GET /ps3mapi.ps3?PROCESS%20GETBASE%200x1010200`?
3. For a CellOS PRX/SPRX, the import stubs are stored in the `.text` section adjacent to the code. Are they placed at a deterministic offset from the ELF load address? E.g., does the import table always follow the `.text` section header?
4. Is there a well-known tool (e.g., `ps3nid`) or SDK function that can convert NID to function address for any loaded PRX? We've been doing manual scanning but it feels like this should be a solved problem.

---

## Question 5: cellUsbd on CFW — LDD Architecture

**Background:**
LEGO Dimensions uses cellUsbd for USB communication with the Toy Pad. Our hook approach intercepts:
- `cellUsbdInit` — return CELL_OK (fake success)
- `cellUsbdOpenPipe` — intercept pipes to Toy Pad endpoints (0x81, 0x01)
- `cellUsbdTransfer` — route Toy Pad IN/OUT via UDP
- `cellUsbdClosePipe` — clean up

We also hook `cellUsbdRegisterLdd` (we have a separate `ldd_driver.c` for this).

**Question:**
1. For USB device emulation on CFW, is cellUsbd hooking the correct approach, or would it be better to use the lower-level `sys_ioctl` / USB LDD (Logical Device Driver) interface that Cobra CFW exposes?
2. We have a `ldd_driver.c` that registers a custom USB driver with `cellUsbdRegisterLdd`. Is this the standard PS3 way to add a virtual USB device, or is there a simpler Tap/TUN-like interface on CFW for USB device emulation?
3. Has anyone successfully emulated a USB HID device from an SPRX on PS3 before? Are there reference projects we should look at?

---

## Question 6: sys_memory_allocate — Executable Pages

**Background:**
We allocate trampoline pages via:
```c
sys_memory_allocate(0x10000, SYS_MEMORY_PAGE_SIZE_64K, &base_addr);
```

**Question:**
1. Is `sys_memory_allocate` guaranteed to return R-W-X pages on CellOS for 64KB allocations? Or do we need to call `sys_memory_set_page_attribute()` afterward to add execute permission?
2. What's the maximum page size we can allocate? We use 64KB but could we use 1MB?
3. Is `SYS_MEMORY_CONTAINER_DEFAULT (0xFFFFFFFF)` always valid for the game process, or could there be memory container permission issues in some games?

---

## Question 7: SO_NBIO Reliability on SDK 3.40 -lnet_stub

**Background:**
We set the socket to non-blocking mode via:
```c
int on = 1;
setsockopt(g_net.socket_fd, SOL_SOCKET, SO_NBIO, (void*)&on, sizeof(on));
```

**Question:**
1. Does `setsockopt(..., SO_NBIO, ...)` actually work with the SDK 3.40 `-lnet_stub` library? We've heard rumors that `SO_NBIO` is unreliable and sometimes the socket stays in blocking mode despite the setsockopt call succeeding.
2. If `SO_NBIO` is unreliable, what's the correct CellOS way to set a socket to non-blocking? Options:
   - `ioctl(fd, FIONBIO, &on)`?
   - Passing `SYS_NET_O_NONBLOCK` to a hypothetical `fcntl()` equivalent?
   - Using `MSG_DONTWAIT` in `recvfrom()` flags?
   - Using `poll()` or `select()` before recvfrom?
3. The `-lnet_stub` library — does it support `poll()` / `sys_net_poll()` or `select()`? We don't have POSIX header support for these.

---

## Question 8: sendto Error Codes on CellOS

**Background:**
When the bridge server isn't running, `sendto()` returns error -2147417535 (0x80010041 = `CELL_NET_ERROR_ENETDOWN`).

**Question:**
1. Is `ENETDOWN` (0x80010041) the expected error when no one is listening on the target port? Or is this a routing issue?
2. Should we get `ECONNREFUSED` (-2147417595 = 0x80010005) instead for UDP with no listener? Or does UDP never return ECONNREFUSED because it's connectionless?
3. Our packet `network_send()` logs `DEBUG_ERROR` on every sendto failure, which would flood the debug log if the server isn't running. Is there a CellOS convention for rate-limiting temporary network errors, or should we just check reachability before each send?
4. Is `connect()` on a UDP socket a viable way to detect server presence before `sendto()`? Or does that have side effects on the socket?

---

## Question 9: Build & Signing with oscetool

**Background:**
We build the SPRX with Sony DUPLEX SDK (SN Systems, SDK 3.40) and sign with oscetool 0.9.2.

Our oscetool flags:
```
-0 SELF -1 TRUE -2 000A -3 1010000001000003 -4 01000002
-A 0001000000000000 -5 APP -8 40000000000000... -6 0004009300000000
```

**Question:**
1. `-5 APP` works (without it we get "Invalid SELF type"). Is `-5 APP` correct for an SPRX that runs in-game, or should it be `-5 ISLAND`? The difference between APP and ISLAND for SPRX signing?
2. Our toolchain uses `-llv2_stub -lfs_stub -lnet_stub`. Are there any known issues with `-lnet_stub` on SDK 3.40 where certain socket operations return wrong error codes or don't work?
3. What's the correct way to compile for the exact firmware version (4.89)? Our SDK is 3.40 — does the SDK version matter, or is it the oscetool signing that determines firmware compatibility?
4. We build in WSL using the Windows SDK binaries (`.exe` files). Are there known issues with paths or permissions when cross-compiling from WSL?

---

## Question 10: Debugging Without a Physical Toy Pad

**Background:**
We can't fully test the hooks because LEGO Dimensions won't attempt USB communication until it detects a Toy Pad. The game is at the "Connect Toy Pad" screen and sits there forever.

**Question:**
1. Is there a way to trick the game into calling cellUsbdInit / cellUsbdOpenPipe early, so we can at least verify our hooks catch those calls? E.g., patching a branch in the game's init code?
2. Does the game call cellUsbdRegisterLdd early in boot (before reaching the "Connect Toy Pad" screen)? We hook this but haven't seen it trigger.
3. For USB LDD emulation on Cobra CFW: does `cellUsbdRegisterLdd` need to be called before `cellUsbdInit`, or is the registration independent of device initialization?
4. Could we write a small test PRX that just calls cellUsbdInit and cellUsbdTransfer to verify the hook chain works, independent of LEGO Dimensions? We have a hello-plugin test project — should we expand it for this?

---

## Code Snippets for Context

### The 4 NID values we search for (`usb_hooks.c:39-43`):
```c
#define NID_CELL_USBD_INIT          0x7F5F00D3U
#define NID_CELL_USBD_OPEN_PIPE     0x1AB6D80BU
#define NID_CELL_USBD_TRANSFER      0x7B4436CEU
#define NID_CELL_USBD_CLOSE_PIPE    0x2F82F1A5U
```

### 4-instruction preamble format (written by Node.js to game .text):
```
[0] lis  r11, hi16(wrapper_addr)    0x3D60xxxx
[1] ori  r11, r11, lo16(wrapper)    0x616Bxxxx
[2] mtctr r11                       0x7D6B03A6
[3] bctr                            0x4E800420
```

### Trampoline format (32 bytes in executable page):
```
[0-3]   4 original instructions from target function (copied)
[4]     lis  r11, hi16(target+16)   0x3D60xxxx
[5]     ori  r11, lo16(target+16)   0x616Bxxxx
[6]     mtctr r11                   0x7D6B03A6
[7]     bctr                        0x4E800420
```

### HOOK_WRAPPER assembly macro (`toc_trampoline.s:41-72`):
```asm
.macro HOOK_WRAPPER asm_name, c_function, toc_reg
    stwu %r1, -0x60(%r1)        /* allocate frame */
    mflr %r0                     /* save LR */
    stw  %r0, 0x64(%r1)
    stw  %r2, 0x28(%r1)         /* save game TOC (r2) */
    lwz  \toc_reg, 0x28(%r1)    /* pass TOC to C function */
    bl   \c_function             /* call C hook */
    lwz  %r2, 0x28(%r1)         /* restore game TOC */
    lwz  %r0, 0x64(%r1)
    mtlr %r0
    addi %r1, %r1, 0x60         /* deallocate frame */
    blr
.endm
```

### HOOK_PASSTHROUGH assembly macro (`toc_trampoline.s:85-130`):
```asm
.macro HOOK_PASSTHROUGH asm_name, num_args
    stwu %r1, -0x60(%r1)
    mflr %r0
    stw  %r0, 0x64(%r1)
    mr   %r2, %r3              /* r3 = game_toc → r2 */
    mr   %r11, %r4             /* r4 = tramp_addr */
    /* shift args: r5+ → r3+ */
    mr   %r3, %r5
    ...                         /* based on num_args */
    mtctr %r11
    bctrl                       /* call trampoline */
    lwz  %r0, 0x64(%r1)
    mtlr %r0
    addi %r1, %r1, 0x60
    blr
.endm
```

### OPD construction in C (`toc_trampoline_c.c:73-89`):
```c
typedef struct {
    uint32_t code_addr;    /* Ptr to .text code */
    uint32_t toc_addr;     /* TOC base value */
    uint32_t env_ptr;      /* Environment pointer */
} ppc_opd_t;

ppc_opd_t g_opd_passthrough_OpenPipe = {
    .code_addr = (uint32_t)(uintptr_t)&asm_passthrough_OpenPipe,
    .toc_addr  = 0,          /* overwritten by passthrough stub */
    .env_ptr   = 0
};

passthrough_openpipe_fn call_original_OpenPipe =
    (passthrough_openpipe_fn)&g_opd_passthrough_OpenPipe;
```

### Socket init (`network.c:55-121`):
```c
ret = sys_net_initialize_network();
// 2-second boot stabilization delay
socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
// Bind retry loop (120 attempts × 100ms)
bind(g_net.socket_fd, ...);
setsockopt(g_net.socket_fd, SOL_SOCKET, SO_BROADCAST, ...);
setsockopt(g_net.socket_fd, SOL_SOCKET, SO_NBIO, ...);
```

### Module init chain (`main.c:157-359`):
```c
debug_init();
network_init(28472);
network_wait_ready();
network_set_server(htonl(0xC0A80011), 28472);  // hardcoded PC IP
usb_hook_init();      // ← currently fails here
toypad_state_init();
// Main loop: heartbeat++, recv, probe, keepalive, sleep 50ms
```

### Injector preamble writing (`inject-sprx.js:418-429`):
```javascript
const preamble = Buffer.alloc(16);
preamble.writeUInt32BE((0x3D60 << 16) | ((wrapperAddr >> 16) & 0xFFFF), 0);  // lis r11
preamble.writeUInt32BE((0x616B << 16) | (wrapperAddr & 0xFFFF), 4);          // ori r11,r11
preamble.writeUInt32BE(0x7D6B03A6, 8);  // mtctr r11
preamble.writeUInt32BE(0x4E800420, 12); // bctr
// Write via MEMORY SET
GET /ps3mapi.ps3?MEMORY%20SET%200xPID%200xADDR%20HEXDATA
```

### PS3/webMAN connectivity confirmed:
```
PS3 IP:   192.168.0.47
webMAN:   MOD 1.47.48c
Game:     LEGO Dimensions BLUS31473 v01.22
Game PID: 0x1010200 (16843264)
Memory:   getmem.ps3mapi works on 0x00100000-0x00A00000
```

---

*End of expert questions. Referenced handoff document: `docs/HANDFF-2026-07-21-DIAGNOSTIC-RESULTS.md`*
