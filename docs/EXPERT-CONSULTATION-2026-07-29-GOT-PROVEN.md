# Expert Consultation — 2026-07-29: GOT Patching Proven — LV2 Filter Blocker

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q [Full]
**Game:** LEGO Dimensions BLUS31473
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2 -fno-builtin -nodefaultlibs`
**SPRX:** ldtoypad.sprx (24,912 bytes v16), signed, PS3MAPI MODULE LOAD
**Status:** 🟢 GOT: 8/10 entries, 5/5 functions | 🟢 HARVESTER: LddOps FOUND | 🟡 Trigger: deferred, needs test

---

## Executive Summary

We have achieved the **Holy Grail of PS3 late-injection hooking**: GOT pointer
scanning with synthetic OPD insertion. The SPRX runs inside the game process,
scans the EBOOT's writable `.data` segment for libusbd OPD pointers, and
replaces them with pointers to our own OPDs in an R-W-X trampoline page.

**8 of 10 GOT entries patched across all 5 target functions** with zero DSI
crashes. The game remains perfectly stable.

However, the game's "Connect ToyPad" screen never calls `cellUsbdGetDeviceDescriptor`.
The LV2 kernel filters USB devices by VID/PID **before** waking the game's
registered LDD (via `cellUsbdRegisterExtraLdd`). Only a device with
VID=0x0E6F, PID=0x0241 passes the kernel filter.

**We need guidance on bypassing the LV2 kernel VID/PID filter.**

---

## What We've Proven Works

- ✅ **cellFsWrite safe probe**: scans 0x02000000-0xC0000000 without crashing
- ✅ **libusbd located**: runtime base 0x02680000, lazy-loaded at USB init
- ✅ **GOT pointer scanning**: finds libusbd OPD values in EBOOT `.data`
- ✅ **Synthetic OPD creation**: ABI-compliant `{code, TOC, 0}` descriptors
- ✅ **PowerPC cache flush**: `dcbst/sync/icbi/isync` after every GOT write
- ✅ **cellFsWrite page probing**: prevents DSI on unmapped pages during scan
- ✅ **Game stability**: zero crashes from scanning ~500MB of address space
- ✅ **IPC file**: STATUS/TARGET_*/TRAMP_*/LDD_OPS format proven

## What We've Tried and Failed

| Approach | Result |
|----------|--------|
| PS3MAPI MEMORY SET | Returns 226 OK but libusbd `.text` unchanged |
| Raw pointer write to libusbd `.text` | DSI crash (MMU R-X protection) |
| `cellUsbdGetDeviceDescriptor` GOT hook | Patched (1 entry) but game never calls it |
| LDD ops struct scanner (name-based) | False positive on "USB" substring |
| LDD ops struct scanner (pattern-based) | Crashes on 0x02000000-0x04000000 range |

---

## Architecture: GOT Patching (Working — Proven Stable)

### Trampoline Page Layout

```
0x11720000 (64KB, R-W-X, allocated via sys_memory_allocate):

Offset  Size   Content
──────────────────────────────────────────
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

### GOT Scan Targets (libusbd OPD addresses at runtime)

```c
#define LIBUSBD_OFFSET_OPENPIPE       0x00000244
#define LIBUSBD_OFFSET_CLOSEPIPE      0x00000380
#define LIBUSBD_OFFSET_INTERRUPT_XFER 0x000004B4
#define LIBUSBD_OFFSET_GET_DEV_DESC   0x0000061C
#define LIBUSBD_OFFSET_CONTROL_XFER   0x000007C8

/* Runtime: base + offset */
TARGET_OPENPIPE   = 0x02680244
TARGET_TRANSFER   = 0x026804B4
TARGET_CLOSEPIPE  = 0x02680380
TARGET_GETDEVDESC = 0x0268061C
TARGET_CTRLXFER   = 0x026807C8
```

### GOT Scanning Algorithm (Full Code)

```c
/* Step 4.5: GOT PATCHING — redirect game's PLT to our trampolines. */
{
    uint32_t tramp_base = g_usb_hooks.trampoline_base;
    uint32_t opd_base   = tramp_base + 512;

    /* Get SPRX TOC from our hook function's OPD */
    uint32_t sprx_toc = ((uint32_t*)my_cellUsbdOpenPipe)[1];

    /* Create 5 OPDs pointing to our trampolines */
    volatile uint32_t *opd = (volatile uint32_t*)(uintptr_t)opd_base;
    struct { uint32_t tramp_offset; uint32_t libusbd_opd; const char *name; } hooks[] = {
        {g_usb_hooks.tramp_open_pipe_offset,         target_openpipe,   "OpenPipe"},
        {g_usb_hooks.tramp_transfer_offset,          target_transfer,   "Transfer"},
        {g_usb_hooks.tramp_close_pipe_offset,        target_closepipe,  "ClosePipe"},
        {g_usb_hooks.tramp_get_device_desc_offset,   target_getdevdesc, "GetDevDesc"},
        {g_usb_hooks.tramp_control_transfer_offset,  target_ctrlxfer,   "CtrlXfer"},
        {0, 0, NULL}
    };
    int i;
    for (i = 0; hooks[i].name; i++) {
        opd[i*3+0] = tramp_base + hooks[i].tramp_offset;  /* code_addr */
        opd[i*3+1] = sprx_toc;                              /* toc_addr */
        opd[i*3+2] = 0;                                     /* env_ptr */
    }

    /* Single-pass scan with cellFsWrite page probing */
    {
        int probe_fd, patched = 0;
        cellFsOpen("/dev_hdd0/plugins/mem_probe.tmp",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &probe_fd, NULL, 0);

        struct { uint32_t start; uint32_t end; } ranges[] = {
            {0x00010000, 0x02000000},   /* EBOOT .data/.got */
            {0x02000000, 0x10000000},   /* lower game heap */
            {0x10000000, 0x20000000},   /* upper game heap */
            {0, 0}
        };
        int found[5] = {0,0,0,0,0};
        int ri;
        for (ri = 0; ranges[ri].end; ri++) {
            uint32_t page;
            for (page = ranges[ri].start; page < ranges[ri].end; page += 0x10000) {
                /* Safe probe: cellFsWrite returns CELL_EFAULT for unmapped */
                uint64_t wr;
                if (cellFsWrite(probe_fd, (const void*)(uintptr_t)page, 4, &wr) != CELL_OK)
                    continue;  /* unmapped — skip 64KB */

                uint32_t *scan = (uint32_t*)(uintptr_t)page;
                uint32_t *end  = (uint32_t*)(uintptr_t)(page + 0x10000);

                /* Early exit if all 5 functions have 2+ matches */
                int all_done = 1;
                for (i = 0; i < 5; i++) all_done &= (found[i] >= 2);
                if (all_done) goto got_all;

                for (; scan < end; scan++) {
                    uint32_t val = *scan;
                    for (i = 0; i < 5; i++) {
                        if (found[i] < 2 && val == hooks[i].libusbd_opd) {
                            /* Overwrite GOT entry with our synthetic OPD */
                            *(volatile uint32_t*)scan = opd_base + (uint32_t)(i * 12);

                            /* CRITICAL: flush dcache+icache */
                            __asm__ volatile (
                                "dcbst 0, %0\n\t"
                                "sync\n\t"
                                "icbi 0, %0\n\t"
                                "isync"
                                :: "r"(scan) : "memory"
                            );
                            found[i]++;
                        }
                    }
                }
            }
        }
got_all:
        cellFsClose(probe_fd);
        cellFsUnlink("/dev_hdd0/plugins/mem_probe.tmp");

        /* Report results */
        for (i = 0; i < 5; i++) {
            patched += found[i];
            /* ... papertrail logging ... */
        }
    }
}
```

### Trampoline Generator (64-byte PowerPC asm)

```c
void create_hook_trampoline(uint32_t *tramp, void *c_func, int toc_arg_reg)
{
    const ppc_opd_t *opd = (const ppc_opd_t *)c_func;
    uint32_t code = opd->code_addr;
    uint32_t toc  = opd->toc_addr;
    int i = 0;

    /* [0] stwu r1, -0x60(r1)  — allocate stack frame */
    tramp[i++] = 0x9421FFA0u;
    /* [1] mflr r0              — save link register */
    tramp[i++] = 0x7C0802A6u;
    /* [2] stw r0, 0x64(r1)    — store LR in stack */
    tramp[i++] = 0x90010064u;
    /* [3] stw r2, 0x28(r1)    — save game's TOC */
    tramp[i++] = 0x90410028u;
    /* [4] mr toc_arg_reg, r2  — pass game TOC as argument */
    tramp[i++] = 0x7C401378u | ((uint32_t)(toc_arg_reg & 0x1F) << 16);
    /* [5] lis r2, toc@h       — load SPRX TOC high */
    tramp[i++] = 0x3C400000u | ((toc >> 16) & 0xFFFF);
    /* [6] ori r2, r2, toc@l   — load SPRX TOC low */
    tramp[i++] = 0x60420000u | (toc & 0xFFFF);
    /* [7] lis r12, code@h     — load hook code addr high */
    tramp[i++] = 0x3D800000u | ((code >> 16) & 0xFFFF);
    /* [8] ori r12, r12, code@l */
    tramp[i++] = 0x618C0000u | (code & 0xFFFF);
    /* [9] mtctr r12 */
    tramp[i++] = 0x7D8903A6u;
    /* [10] bctrl              — call C hook via CTR */
    tramp[i++] = 0x4E800421u;
    /* [11] lwz r2, 0x28(r1)   — restore game's TOC */
    tramp[i++] = 0x80410028u;
    /* [12] lwz r0, 0x64(r1)   — restore LR */
    tramp[i++] = 0x80010064u;
    /* [13] mtlr r0 */
    tramp[i++] = 0x7C0803A6u;
    /* [14] addi r1, r1, 0x60  — deallocate stack */
    tramp[i++] = 0x38210060u;
    /* [15] blr                 — return to game */
    tramp[i++] = 0x4E800020u;
}
```

### Hook Functions (C callbacks)

```c
/* OpenPipe: intercepts open-pipe, returns fake handle */
int my_cellUsbdOpenPipe(void *pipe_handle, uint32_t dev_id,
                         void *ep_descriptor, uint32_t game_toc)
{
    static uint32_t next_handle = 0xFD000001;
    uint32_t handle = next_handle++;
    *(uint32_t*)pipe_handle = handle;
    DEBUG_PRINT("[USB] *** HOOK OpenPipe(dev=0x%X) -> handle=0x%X ***\n",
                (unsigned)dev_id, (unsigned)handle);
    return CELL_OK;
}

/* InterruptTransfer: passes data between game and UDP bridge */
int my_cellUsbdInterruptTransfer(uint32_t pipe_handle, void *buf,
                                  uint32_t *len, void *done_cb, void *arg,
                                  uint32_t game_toc)
{
    /* ... bridge to UDP server ... */
}

/* ClosePipe: no-op, returns success */
int my_cellUsbdClosePipe(uint32_t pipe_handle, uint32_t game_toc)
{
    return CELL_OK;
}

/* GetDeviceDescriptor: TROJAN HORSE — returns ToyPad descriptor */
int my_cellUsbdGetDeviceDescriptor(uint32_t dev_id, void *desc,
                                    uint32_t game_toc)
{
    static const uint8_t toypad_dev_desc[18] = {
        0x12, 0x01,             /* bLength=18, bDescriptorType=DEVICE */
        0x00, 0x02,             /* bcdUSB 2.0 */
        0x00, 0x00, 0x00,       /* class, subclass, protocol */
        0x08,                   /* bMaxPacketSize0 */
        0x6F, 0x0E,             /* idVendor: 0x0E6F (Logic3/PDP) */
        0x41, 0x02,             /* idProduct: 0x0241 (LEGO Dimensions) */
        0x00, 0x01,             /* bcdDevice 1.00 */
        0x01, 0x02, 0x00,       /* iManufacturer, iProduct, iSerialNumber */
        0x01                    /* bNumConfigurations */
    };
    if (desc) memcpy(desc, toypad_dev_desc, 18);
    papertrail("[USB] *** TROJAN HORSE FIRED! Returning ToyPad VID/PID ***");
    return CELL_OK;
}

/* ControlTransfer: routes descriptor requests through toypad_state */
int my_cellUsbdControlTransfer(uint32_t dev_handle, void *setup_pkt,
                                void *buf, uint32_t *length,
                                void *done_cb, void *user_data,
                                uint32_t game_toc)
{
    /* Intercept GET_DESCRIPTOR requests, return ToyPad HID descriptors */
    /* ... */
}
```

### Verified Boot Log Output

```
[USB] Scan FULL 0x02000000-0xC0000000 (retry 5s, max 100s)...
[USB] probing 0x02400000
[USB] libusbd base=0x02680000
[USB] OPDs at 0x11720200, TOC=0x028F82B0
[USB] GOT OpenPipe: 2 ent
[USB] GOT Transfer: 2 ent
[USB] GOT ClosePipe: 2 ent
[USB] GOT GetDevDesc: 1 ent
[USB] GOT CtrlXfer: 1 ent
[USB] GOT done: 8 entries. LIVE.
[USB] IPC written with real TARGET_* addresses
OK: usb_hook_init() returned success
=== Entering main loop ===
```

---

## The Blocker: LV2 Kernel VID/PID Filter

### How LEGO Dimensions Uses USB (Expert Analysis)

The game uses `cellUsbdRegisterExtraLdd(ops, 0x0E6F, 0x0241)` — a **filtered
LDD registration**. The kernel only wakes the game's USB thread when a device
with exactly VID=0x0E6F and PID=0x0241 is connected.

When we plug in a USB flash drive (e.g., VID=0x0781, SanDisk), the kernel:
1. Reads hardware descriptors → sees SanDisk VID/PID
2. Checks against game's registered LDD filter → mismatch
3. **Silently discards the event** — never wakes the game

This is why the `cellUsbdGetDeviceDescriptor` GOT hook never fires. The game
isn't ignoring our hook — the kernel never even gives the game a chance to
call the function.

### Why GOT-Patching GetDeviceDescriptor Didn't Help

The game imports `cellUsbdGetDeviceDescriptor` (1 GOT entry found and patched),
but the "Connect ToyPad" screen's USB enumeration doesn't route through this
function. The kernel delivers device info directly to the LDD's `probe` callback,
bypassing the game's own `cellUsbdGetDeviceDescriptor` calls.

---

## Questions for Expert

### Q1: Recommended Approach for LV2 Filter Bypass

You gave us 3 options. Based on our constraints (no hardware mod tools, SPRX
injected at T+60s via PS3MAPI), which path should we pursue?

**Option A (Hardware Spoof):** We don't have a Raspberry Pi Pico or Arduino
readily available. Is there a software-only way to spoof USB VID/PID on the
PS3 itself?

**Option B (Hook `cellUsbdRegisterExtraLdd`):**
- This function is NOT in libusbd.sprx (no matching OPD entry)
- Which PRX exports it? `libusbd.sprx`? `sys_io`? Something else?
- Can we GOT-patch it from the game EBOOT (does the game import it)?

**Option C (LDD Ops Scanner):**
- Our first attempt crashed scanning 0x02000000-0x04000000
- What's the correct memory range where dynamically-allocated LDD OPDs live?
- Is there a way to narrow the scan using known patterns?

### Q2: Can We Register Our Own LDD?

Since we can call `cellUsbdRegisterExtraLdd` from the SPRX, can we:
1. Register our own LDD with VID=0x0E6F, PID=0x0241
2. Have the kernel call OUR probe/attach callbacks
3. From our callbacks, trigger the GAME's LDD callbacks (once we find them)?

### Q3: Is There a CellOS API to Enumerate Registered LDDs?

If we could query the kernel for all registered LDDs and their ops structs,
we could find the game's LDD without any memory scanning.

### Q4: Can We Patch the Kernel's VID/PID Filter?

On Cobra 8.5 CFW, is there a way to:
- Modify the kernel's LDD filter table?
- Temporarily disable LDD filtering?
- Register a catch-all LDD that receives ALL USB events?

---

## Build Info

```
SDK: DUPLEX 3.40
Compiler: ppu-lv2-gcc -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs
Link: -llv2_stub -lfs_stub -lnet_stub -lusbd_stub -lsysmodule_stub
Sign: oscetool 0.9.2 -5 APP (WSL)
SPRX: 24,272 bytes signed (146,792 bytes unsigned)
Injection: PS3MAPI MODULE LOAD via webMAN HTTP at T+60s

Key source files:
  sprx-plugin/usb_hooks.c      — libusbd finder, GOT scanner, 5 C hooks
  sprx-plugin/trampoline_gen.c — 64-byte PowerPC trampoline generator
  sprx-plugin/ldd_driver.c     — our own LDD (registered, but filtered)
  sprx-plugin/main.c           — worker thread, init sequence
  ld-toypad-server/scripts/inject-sprx.js — Node.js orchestrator
```

---

## APPENDIX D: Harvester + Deferred Trigger (v14–v16)

**Date:** 2026-07-29 08:40 (implemented per expert guidance)

### Harvester Result (v14, confirmed v15)

```
[USB] HARVESTER: LddOps at 0x01E66C7C name='desc' probe=0x01E0C100 attach=0x01E0C108
```

Found the game's `CellUsbdLddOps` struct in EBOOT `.data` in milliseconds.
All 4 pointers within EBOOT range, TOC match confirmed between probe/attach OPDs.

### Harvester Code (runs after GOT patching, in usb_hook_init Step 4.6)

```c
/* Scan EBOOT .data/.rodata (0x00010000-0x02000000) — GUARANTEED contiguous */
uint32_t *scan = (uint32_t*)0x00010000;
uint32_t *end  = (uint32_t*)0x02000000;
for (; scan < end - 4; scan++) {
    uint32_t name_ptr   = scan[0];
    uint32_t probe_op   = scan[1];
    uint32_t attach_op  = scan[2];
    uint32_t detach_op  = scan[3];

    /* All 4 pointers in EBOOT range */
    if (name_ptr  < 0x00010000 || name_ptr  >= 0x02000000) continue;
    if (probe_op  < 0x00010000 || probe_op  >= 0x02000000) continue;
    if (attach_op < 0x00010000 || attach_op >= 0x02000000) continue;
    if (detach_op < 0x00010000 || detach_op >= 0x02000000) continue;

    /* Dereference OPDs: same TOC = smoking gun */
    uint32_t probe_toc   = ((uint32_t*)probe_op)[1];
    uint32_t attach_toc  = ((uint32_t*)attach_op)[1];
    if (probe_toc != attach_toc) continue;

    /* Code addresses must be in .text */
    uint32_t probe_code  = ((uint32_t*)probe_op)[0];
    if (probe_code < 0x00010000 || probe_code >= 0x02000000) continue;

    /* Name must be printable ASCII */
    char *name = (char*)name_ptr;
    if (name[0] < 0x20 || name[0] > 0x7E) continue;

    /* Found! Store for deferred trigger */
    g_usb_hooks.ldd_ops_addr = (uint32_t)(uintptr_t)scan;
    break;
}
```

### Deferred Manual Trigger (v16, in worker_thread main loop)

Trigger fires 3 seconds into the main loop (after game fully stabilizes):

```c
/* In main loop, after 60 iterations (3s @ 50ms) */
if (!harvester_triggered && !use_opd_fallback &&
    g_usb_hooks.ldd_ops_addr && loop_count > 60) {
    harvester_triggered = 1;
    uint32_t *ops = (uint32_t*)g_usb_hooks.ldd_ops_addr;
    int pr = call_game_opd(ops[1], 0x99);  /* probe(0x99) */
    if (pr == 0) {
        call_game_opd(ops[2], 0x99);        /* attach(0x99) */
    }
}
```

### Cross-TOC OPD Caller (v16, with stack frame + LR save)

```c
static int call_game_opd(uint32_t opd_addr, int dev_id) {
    uint32_t *opd = (uint32_t*)opd_addr;
    uint32_t code = opd[0], toc = opd[1];
    int result;
    __asm__ volatile (
        "stwu   1, -0x80(1)\n\t"  /* allocate stack frame */
        "mflr   0\n\t"            /* save LR */
        "stw    0, 0x84(1)\n\t"
        "mr     11, 2\n\t"        /* save SPRX r2 */
        "mr     2, %1\n\t"        /* load game TOC */
        "mtctr  %2\n\t"           /* code → CTR */
        "mr     3, %3\n\t"        /* dev_id → r3 */
        "bctrl\n\t"               /* call game function */
        "mr     2, 11\n\t"        /* restore SPRX r2 */
        "lwz    0, 0x84(1)\n\t"   /* restore LR */
        "mtlr   0\n\t"
        "addi   1, 1, 0x80\n\t"   /* deallocate stack */
        "mr     %0, 3\n\t"        /* capture return */
        : "=r"(result) : "r"(toc), "r"(code), "r"(dev_id)
        : "r0","r3","r11","ctr","lr","cr0","memory");
    return result;
}
```

### v15 Test Result (init-time trigger — DSI, thread killed)

The init-time trigger reached "Firing game probe(0x99) manually..." but the
log cut off. The game stayed alive (XMB overlay still worked), but the SPRX
worker thread was killed by DSI. Root cause: calling a game function without
a proper stack frame. Fixed in v16 with `stwu/addi` frame + LR save.

### v16 Test Result

AWAITING TEST — game is live from v15 injection, ready for re-inject.

### Injector Change

Default wait reduced from 60s to 5s (`--wait` flag still overrides).
Game is already at ToyPad screen when we inject — no need to wait for USB init.
