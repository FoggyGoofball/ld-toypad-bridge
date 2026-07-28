# Expert Consultation — 2026-07-28: libusbd FOUND — Final Blocker (HANDOFF v5)

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q [Full]
**Game:** LEGO Dimensions BLUS31473
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2 -fno-builtin -nodefaultlibs`
**SPRX:** ldtoypad.sprx (23,472 bytes v11), signed, PS3MAPI MODULE LOAD
**Status:** 🟢 libusbd FOUND at 0x02680000 | 🔴 Cannot write to .text

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
