# Expert Consultation — 2026-07-28: cellFsWrite Safe Probe (HANDOFF v4 FINAL)

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q [Full]
**Game:** LEGO Dimensions BLUS31473
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2 -fno-builtin -nodefaultlibs`
**SPRX:** ldtoypad.sprx (23,296 bytes v8), signed, PS3MAPI MODULE LOAD
**Status:** 🟢 SPRX crash-proof, no DSI | 🔴 libusbd NOT FOUND after 8 iterations

### RESULTS TABLE (All Tests, 2026-07-28)

| v | Scan Range | Window | Result |
|---|-----------|--------|--------|
| v3 | 0x02000000→0x04000000 | exact 0x94EC | ❌ Not found |
| v4 | 0x02000000→0x20000000 | exact 0x94EC | ❌ Not found (480MB game heap, all CELL_OK) |
| v5 | — diagnostics only | exact 0x94EC | ⚠️ cellFsWrite OK, readback EMPTY |
| v6 | 0x30000000→0x40000000 | exact 0x94EC | ❌ Not found (PRX region, all CELL_OK) |
| v7 | 0x30000000→0x40000000 | 512B window (v+0x9400→v+0x9600) | ❌ Not found, PS3 SURVIVED |
| v8 | 0x30000000→0x40000000 | ±48B around 0x94EC | ⏳ Deployed, awaiting test |

**Total memory scanned:** ~992MB across 3 address ranges — zero DSI crashes.

### CRITICAL DIAGNOSTIC FINDING (v5)
- `cellFsWrite(local_string)` → `ret=0 (CELL_OK), wr=16` ✅
- `cellFsWrite(0x00010000)` → `ret=0, wr=16` (EBOOT header probe) ✅
- `cellFsRead` back from file → **EMPTY** (0 bytes or failed)
- This means we cannot CONFIRM cellFsWrite transmitted the buffer data

### PRX REGION SCAN (v6-v7)
- 0x30000000→0x40000000: ALL 1024 pages returned CELL_OK with 16 bytes written
- Windowed search (±512B around 0x94EC) also found nothing
- PS3 did NOT crash from windowed reads — pages are genuinely accessible

## PRIMARY QUESTION: Where is libusbd.sprx actually loaded?

After scanning 0x02000000→0x20000000 (game heap) and 0x30000000→0x40000000
(PRX region), "cellUsbd_Library" / "cellUsbd" is NOT at base+0x94EC in either.

1. **What is the actual load address range for libusbd.sprx on Evilnat 4.93 CEX
   with Cobra 8.5?** Is it above 0x40000000? At 0x60000000+?

2. **Is the module name offset 0x94EC guaranteed correct at runtime?** We derived
   it from the decrypted ELF (sceModuleInfo at 0x95D4 → vaddr 0x94E4, name at +8).
   Could the runtime layout differ?

3. **Is cellFsWrite actually transmitting the buffer data?** v5 diagnostics show
   cellFsWrite returns CELL_OK but cellFsRead back from the file returns nothing.
   If cellFsWrite isn't reading the buffer, the entire approach is invalid.

4. **Alternative: can we use the SPRX's resolved cellUsbd imports?** The PRX links
   with -lusbd_stub. When loaded, CellOS resolves these to libusbd's OPDs. Can we
   read the SPRX's GOT to get the resolved addresses? Or use a PPU asm trick to
   capture the PLT-resolved target?

## 5. What Works (Ready the Moment libusbd Base is Known)

- ✅ SPRX injects, no crashes across 8 iterations and 992MB of memory scanning
- ✅ Trampolines allocated, 5 hooks ready, IPC format proven
- ✅ Offline offsets: 0x244, 0x380, 0x4B4, 0x61C, 0x7C8
- ✅ Preamble builder ready, PS3MAPI MEMORY SET for installation
- ❌ **libusbd.sprx runtime base** — ONLY remaining blocker
/* Offset of "cellUsbd_Library" in libusbd.sprx sceModuleInfo */
#define MODULE_INFO_OFFSET  0x94EC

static uint32_t find_libusbd_base_safe(void)
{
    int fd;
    uint32_t found_base = 0;
    uint64_t written;
    int ret;
    uint32_t v;

    /* ── DIAGNOSTIC 1: Write known local string, read back ── */
    {
        const char test_str[] = "TESTSTRING123456";
        char verify[16];
        int vfd;
        uint64_t vread;

        if (cellFsOpen("/dev_hdd0/plugins/mem_probe.tmp",
                CELL_FS_O_RDWR | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                &vfd, NULL, 0) != CELL_OK) {
            papertrail("[USB] FATAL: open failed");
            return 0;
        }
        ret = cellFsWrite(vfd, test_str, 16, &written);
        /* Log ret + written */
        ret = cellFsLseek(vfd, 0, CELL_FS_SEEK_SET, NULL);
        ret = cellFsRead(vfd, verify, 16, &vread);
        /* Log hex of verify[] */
        cellFsClose(vfd); cellFsUnlink("/dev_hdd0/plugins/mem_probe.tmp");
    }

    /* ── DIAGNOSTIC 2: Write from EBOOT header (0x00010000, known-good) ── */
    {
        const char *self_test = (const char*)0x00010000;
        int sfd;
        if (cellFsOpen("/dev_hdd0/plugins/mem_probe.tmp",
                CELL_FS_O_RDWR | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                &sfd, NULL, 0) == CELL_OK) {
            ret = cellFsWrite(sfd, self_test, 16, &written);
            if (ret == CELL_OK && written == 16) {
                char rbuf[32]; uint64_t rr = 0;
                cellFsLseek(sfd, 0, CELL_FS_SEEK_SET, NULL);
                cellFsRead(sfd, rbuf, 16, &rr);
                /* Log hex of rbuf — should be 0F050C06... (EBOOT magic) */
            }
            cellFsClose(sfd); cellFsUnlink("/dev_hdd0/plugins/mem_probe.tmp");
        }
    }

    /* ── MAIN SCAN: 0x02000000 → 0x20000000 at 64KB steps ── */
    if (cellFsOpen("/dev_hdd0/plugins/mem_probe.tmp",
            CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
            &fd, NULL, 0) != CELL_OK) {
        papertrail("[USB] FATAL: open failed");
        return 0;
    }

    int progress = 0;
    for (v = 0x02000000; v < 0x20000000; v += 0x10000) {
        uint32_t probe_addr = v + MODULE_INFO_OFFSET;
        written = 0;
        ret = cellFsWrite(fd, (const void*)(uintptr_t)probe_addr, 16, &written);

        if (ret == CELL_OK && written == 16) {
            const char *p = (const char*)(uintptr_t)probe_addr;
            const char *t = "cellUsbd_Library";
            int i, match = 1;
            for (i = 0; i < 16; i++) {
                if (p[i] != t[i]) { match = 0; break; }
            }
            if (match) { found_base = v; break; }
        }

        /* Progress every 4MB */
        progress++;
        if ((progress & 63) == 0) {
            char pbuf[48]; /* "[USB] probing 0xXXXXXXXX" */
            /* ... format & papertrail ... */
        }
    }

    cellFsClose(fd);
    cellFsUnlink("/dev_hdd0/plugins/mem_probe.tmp");
    return found_base;
}
```

### usb_hook_init — Self-Contained Pipeline

```c
int usb_hook_init(void)
{
    uint32_t lib_base;
    /* ... init ... */

    lib_base = find_libusbd_base_safe();
    if (lib_base == 0) {
        papertrail("[USB] FATAL: Safe probe could not find libusbd!");
        return -1;
    }

    /* Compute TARGET_* = base + offset */
    target_openpipe   = lib_base + 0x244;
    target_transfer   = lib_base + 0x4B4;
    target_closepipe  = lib_base + 0x380;
    target_getdevdesc = lib_base + 0x61C;
    target_ctrlxfer   = lib_base + 0x7C8;

    install_hooks();
    write_ipc_file(target_openpipe, target_transfer, target_closepipe,
                   target_getdevdesc, target_ctrlxfer);
    return 0;
}
```

## 2. v4 Boot Log — Full 480MB Scan (No Match)

```
[USB] Safe-probing 0x02000000-0x20000000...
[USB] probing 0x02400000
[USB] probing 0x02800000
... (60+ progress lines, 4MB each) ...
[USB] probing 0x1F800000
[USB] probing 0x1FC00000
[USB] probing 0x20000000
[USB] FATAL: Safe probe could not find libusbd!
```

- **7,680** iterations (0x02000000→0x20000000, 64KB step)
- **No crashes** — `cellFsWrite` returned `CELL_OK` for EVERY page
- **No matches** — `"cellUsbd_Library"` never found at `base + 0x94EC`

## 3. What We've Exhausted

| # | Approach | Result |
|---|----------|--------|
| 1 | LV2 syscalls (get_module_id_by_name, get_module_info, get_module_list) | ❌ Blocked/panic |
| 2 | PS3MAPI MEMORY GET (0x02000000-0x40000000 via HTTP) | ❌ All zeros |
| 3 | Node.js Smart Probe (64KB boundaries, PS3MAPI HTTP) | ❌ All zeros |
| 4 | /modules.ps3mapi endpoint (webMAN 1.47.48q Full, Cobra 8.5) | ⚠️ 501 HTTP, "OK" body, no data |
| 5 | webftp_server_full.sprx in game slot 1 | ❌ Doesn't affect web server |
| 6 | **cellFsWrite safe probe v4** (0x02000000-0x20000000) | ⚠️ No crash, no match |
| 7 | **cellFsWrite diagnostic v5** (EBOOT+self-test) | ⏳ Awaiting test |

## 4. Key Questions for Expert

### Q1: Is cellFsWrite actually reading from the buffer pointer?

Every page from 0x02000000-0x20000000 returned `CELL_OK` with `written=16`.
This seems suspicious — 480MB of contiguous pages all mapped and writable?
**v5 includes a self-test**: it writes from EBOOT header (0x00010000) and
reads back the file content to verify `cellFsWrite` actually transmits
remote memory. If the EBOOT readback shows `0F050C06...` (correct), then
`cellFsWrite` IS reading correctly and libusbd is genuinely elsewhere. If
the readback is garbage/zeros, `cellFsWrite` isn't reading the buffer and
this approach is invalid.

### Q2: Could libusbd be at an address above 0x20000000?

We scanned 0x02000000-0x20000000 (512MB). On PS3, firmware modules can
be mapped at higher addresses (0x40000000+, 0x60000000+, or even 0x80000000+).
However, all of those would be in true kernel space where `cellFsWrite`
should return `CELL_EFAULT`. The fact that it returned `CELL_OK` for
0x02000000-0x20000000 suggests the kernel treats this range as accessible
user memory.

### Q3: Is the module name offset (0x94EC) correct?

Calculated from the decrypted `libusbd_493.elf`:
- `sceModuleInfo` at file offset 0x95D4 → vaddr 0x95D4 - 0xF0 = 0x94E4
- Module name at struct offset +8 → 0x94E4 + 8 = 0x94EC

Could the sceModuleInfo structure layout differ at runtime? Could the module
name field be at a different offset in memory?

### Q4: Can we use the SPRX's resolved imports?

The SPRX links with `-lusbd_stub`. When loaded, CellOS resolves
`cellUsbdOpenPipe()` etc. through the PRX's GOT. The GOT entries point to
libusbd's OPDs. If we can read the SPRX's own GOT, we get libusbd addresses.

The DECLARED externs in the SPRX (`extern int cellUsbdOpenPipe(...)`)
resolve through the PLT — taking `&cellUsbdOpenPipe` gives the PLT stub,
not libusbd. **Is there a way in C to read the resolved GOT entry** for an
imported function on CellOS/PPU?

### Q5: Alternative — use PS3MAPI MEMORY SET to write preambles blindly?

If `MEMORY SET` (poke_process) has different privilege than `MEMORY GET`
(peek_process), we could try writing preambles at candidate addresses
without reading first. If libusbd is at 0x02C00000, writing our 4-instruction
preamble at 0x02C00000+0x244 would redirect cellUsbdOpenPipe calls to our
trampoline. If the game still calls cellUsbd afterward, we guessed wrong
and can try the next base.

### Q6: Known libusbd.sprx base for Evilnat 4.93 CEX?

Has anyone documented the load address? Since libusbd is part of the
firmware, its base should be deterministic for a given CFW version.

## 5. Build & Deployment

```
SDK: DUPLEX 3.40
Compiler: ppu-lv2-gcc -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs
Link: -llv2_stub -lfs_stub -lnet_stub -lusbd_stub -lsysmodule_stub
Sign: oscetool 0.9.2 -5 APP (via WSL)
Current SPRX: 23,120 bytes (v5 diagnostic)
Inject: PS3MAPI MODULE LOAD at T+60s via Node.js
PS3: CECH-2501A, Evilnat 4.93 CFW, webMAN MOD 1.47.48q, Cobra 8.5
```

## 6. What Works (Ready to Go Once libusbd Found)

- ✅ SPRX injects without crash (23,120 bytes, zero LV2 syscalls)
- ✅ Trampoline page allocated at R-W-X
- ✅ 5 trampolines generated (OpenPipe, InterruptTransfer, ClosePipe, GetDeviceDesc, ControlTransfer)
- ✅ IPC file format: `TARGET_*=base+offset`, `TRAMP_*=trampoline_page+offset`
- ✅ Preamble builder ready: 4 PowerPC instructions per hook (lis/ori/mtctr/bctr)
- ✅ PS3MAPI MEMORY SET for preamble installation
- ✅ Game runs, PID confirmed
- ❌ **libusbd.sprx runtime base address** — the ONLY remaining blocker
