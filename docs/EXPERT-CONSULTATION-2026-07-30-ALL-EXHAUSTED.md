# Expert Consultation — 2026-07-30: EBOOT Patch + HDD Boot Strategy

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q [Full]
**Game:** LEGO Dimensions BLUS31473 (disc-based, now converting to HDD JB folder)
**SDK:** DUPLEX SDK 3.40, ppu-lv2-gcc, `-mprx -std=gnu99 -O2 -fno-builtin -nodefaultlibs`
**SPRX:** ldtoypad.sprx v24 (25,744 bytes), signed, PS3MAPI MODULE LOAD
**Status:** 🔴 Freezes at update screen — both JB folder AND ISO | ⬜ EBOOT signing suspected

---

## New Issue: Modified EBOOT Freezes at "Update Available" Screen (Both Formats)

**Symptom:** The game freezes at the "A new version of the game is available" prompt with a hard lock — no button input works. This happens identically in BOTH `JB folder` format AND `ISO` format (makeps3iso, no IRD).

**Eliminated causes:**
- ~~Filesystem redirection~~ — ISO format uses block-level emulation, freeze persists
- ~~Stale install data~~ — deleted all BLUS31473 folders from `/dev_hdd0/game/`
- ~~Cobra plugins~~ — tested with L2 held during boot (disables Cobra)
- ~~ISO structure~~ — makeps3iso confirmed, disc structure is intact

**Conclusion:** The freeze is caused by the **patched EBOOT.BIN itself**. The update prompt screen appears to be the point where the PS3 OS or the game performs an integrity check on the SELF file. Our `oscetool -5 APP` re-signing is failing this check.

**What changed:**
1. Original EBOOT.BIN decrypted to ELF → hex-patched (8 bytes: VID + PID) → re-signed with `oscetool -5 APP`
2. Resulting EBOOT is 12.3 MB vs original 28.5 MB (oscetool doesn't reproduce Sony's proprietary compression)
3. Freeze occurs at identical point in both JB folder and ISO deployment

## Updated Questions for Expert

### Q1: What's the correct oscetool signing for this game?

We used `oscetool 0.9.2 -5 APP`. But LEGO Dimensions may require a different self type:
- `-0` = fake/self (no encryption, for debug)
- `-1` = retail self
- `-5` = APP type (what we used — may be wrong for this game)

The original encrypted EBOOT.BIN was 28.5 MB and likely used Sony's proprietary SELF compression. oscetool produces a 12.3 MB uncompressed SELF. Does this size/compression change trigger the integrity check? Should we try `-0` (fake self) which bypasses all signing, or is there a way to preserve the original SELF structure and only patch the 8 bytes in-place without decrypting/re-signing?

The EBOOT was signed with `oscetool 0.9.2 -5 APP` which uses APP-type self-signing. On CFW with Cobra, self-signed EBOOTs should bypass signature checks. But does the update-check screen trigger an additional integrity verification (hash check, SELF header validation) that a self-signed EBOOT fails? Would using `-1` (retail) or `-0` (debug) make a difference?

### Q2: Could old install data be conflicting?

LEGO Dimensions has install data (INSTALL0_*.DAT totaling ~160 MB, plus GAME*.DAT). Previous testing with the disc-based game installed data on the internal HDD. When the patched ISO boots, the PS3 may detect a mismatch between the stored install data and the modified EBOOT. Should we delete `/dev_hdd0/game/BLUS31473INSTALLDATA/` (or wherever the game stores its install) before booting the patched ISO?

### Q3: Does makeps3iso without IRD omit something critical?

The original disc ISO is 22.60 GB, our rebuilt ISO is 22.33 GB (276 MB smaller). We traced this to: PS3_UPDATE (256 MB firmware blob stripped) + EBOOT shrink (16 MB due to oscetool vs Sony compression). Are there other structural elements — like a DISC_KEY, content ID tables, or license files — that makeps3iso strips and the game's update-check code expects to find?

### Q4: Is this a simple PARAM.SFO version mismatch?

The PARAM.SFO in the ISO still has the original disc version number. The update prompt appears because PSN has a newer version. If the game's update-check routine compares the EBOOT hash/version against PARAM.SFO and finds a mismatch, it might lock up. Should we hex-edit PARAM.SFO to bump the version, or disable the update check?

### Q5: Practical fix — delete old data and retry?

The most common cause of modified EBOOT freezes is stale install data. The test protocol should be:
1. Delete any existing BLUS31473 install data from `/dev_hdd0/game/`
2. Boot ISO → press Circle/Cancel at update prompt immediately
3. If freeze persists, try launching while holding L2 (disables Cobra plugins) or R2 (disables webMAN features)

---

## What Works (Proven)

| System | Status |
|--------|--------|
| GOT pointer scanning | ✅ 14/14 entries (8 hooks × 2 each), zero crashes |
| libusbd base finder | ✅ 0x02680000 confirmed across 10+ sessions |
| Synthetic OPD creation | ✅ ABI-compliant {code, TOC, 0} |
| PowerPC cache flush | ✅ dcbst/sync/icbi/isync |
| IPC file format | ✅ Proven, read by Node.js orchestrator |
| Registration hooks | ✅ 3 separate hooks, ppc_opd_t passthrough, expert-reviewed |
| call_game_opd (v17+) | ✅ Stack-save at 0x28, full clobber list |
| **EBOOT VID/PID patch** | ✅ Offline hex edit: ToyPad→SanDisk (0x0E6F/0x0241→0x0781/0x5581) |
| **EBOOT signing** | ✅ oscetool 0.9.2 -5 APP via WSL |
| **ISO → JB folder extraction** | ✅ Windows ISO mount + robocopy to C:\temp\BLUS31473 |
| **FTP to PS3 HDD** | 🔄 In progress: 35/41 files, ~2 MB/s, ~2h remaining |

## New Approach: Bypass Kernel VID/PID Filter via EBOOT Patch

The kernel's USB filter blocks all non-approved VID/PID pairs. By patching the EBOOT to request a SanDisk VID/PID instead of the ToyPad one, the kernel will deliver the device to the game's LDD. The bridge server emulates a SanDisk USB device on the PC side.

### EBOOT Patch Details

| Parameter | Original (ToyPad) | Patched (SanDisk) |
|-----------|-------------------|-------------------|
| VID instruction | `li r4, 0x0E6F` (38A00241) | `li r4, 0x0781` (38A00781) |
| PID instruction | `ori r4, r4, 0x0241` (63440000) | `ori r4, r4, 0x5581` (38800781) |
| File offset VID | 0x286DF0 | ← patched |
| File offset PID | 0x286DEC | ← patched |

### Deployment Chain

1. Mount disc ISO on Windows → extract PS3_GAME folder (22.33 GB)
2. Replace USRDIR\EBOOT.BIN with oscetool-signed patched version (12.3 MB)
3. FTP entire JB folder to `/dev_hdd0/GAMES/BLUS31473/` on PS3
4. Launch from webMAN XMB Games menu → game loads patched EBOOT from HDD
5. Game registers LDD with SanDisk VID/PID → kernel delivers matching USB device
6. Bridge server on PC emulates SanDisk → GOT hooks fire → complete flow

## What Failed Previously

| Method | Result | Why |
|--------|--------|-----|
| **XMB rescan** (v20) | Registration hooks never fire | Game doesn't call RegisterLdd after overlay — only at startup |
| **USB hotplug** (v20) | GetDeviceDescriptor never called | Kernel VID/PID filter blocks device before reaching game |
| **Harvester v17–v24** | ZERO valid ops candidates | CellUsbdLddOps has non-standard layout in TT Games EBOOT |
| **PS3MAPI MEMORY GET** | Returns all zeros | Can't read game process memory via webMAN API |
| **PS3MAPI MEMORY SET** | Error 451 | Can't write game process memory via webMAN API |
| **webMAN copy endpoint** | Error "Not found" | /copy.ps3 doesn't support disc→HDD copy |
| **Disc EBOOT override** | Disc games ignore HDD EBOOT | Must run game from HDD (JB folder), not disc |

## Key Finding

**The `CellUsbdLddOps` struct does NOT follow the standard Sony layout in this EBOOT.** After scanning all 32MB with four different strategies, the ONLY 4-pointer TOC-matched candidate is a USB descriptor table with name="desc". The real ops struct must have a non-standard arrangement.

## Updated Questions for Expert

### Q1: Will the kernel actually deliver a SanDisk USB device to the game?

The EBOOT patch changes the VID/PID the game's LDD registers for. When a matching USB device connects (our bridge emulating SanDisk), the kernel should deliver it. But does the PS3 kernel do anything special for the ToyPad VID/PID (0x0E6F/0x0241) beyond the standard filter? Is there any additional security check — like requiring a specific USB class or endpoint configuration — that could still block us?

### Q2: Does the LDD probe/attach flow require the real ops struct?

Our GOT hooks intercept `cellUsbdRegisterLdd` and capture the ops pointer. But since we can't find the struct in the EBOOT, we currently report `ldd_ops_addr = 0x0`. When the kernel delivers a device and calls `probe`/`attach` through libusbd, those call into the game's real ops. Our GOT hooks on OpenPipe/Transfer/ClosePipe/etc. only fire AFTER probe/attach succeed. If probe/attach don't fire (or fail), we never reach our hooks. Is there a way to trigger probe/attach without the real ops, or to intercept the probe/attach call chain ourselves?

### Q3: Can we force the kernel to rescan without hotplugging?

Once the game is running with the patched EBOOT, the LDD is registered waiting for a SanDisk device. But the USB device (our bridge) might already be connected before the game starts. Does the PS3 kernel deliver already-connected devices to newly registered LDDs? Or do we need to physically disconnect/reconnect the USB to trigger delivery? Is there a VSH syscall or a libusbd function we could hook to force re-enumeration?

### Q4: What's the cleanest way to get the ops pointer from libusbd internals?

We still need the ops address for completeness (and for any future direct callback manipulation). Since libusbd stores the registered LDD list internally at 0x02680000, what's the known offset or structure for the LDD registry? Is it a linked list? A fixed array? If we could walk libusbd's internal state, we could find the ops pointer without scanning the EBOOT at all.

### Q5: If the EBOOT patch works, what's the expected hook order?

With SanDisk VID/PID in the EBOOT and a bridge server emulating a SanDisk device, what's the expected sequence?
1. Game boots → calls `cellUsbdRegisterLdd(SanDisk_ops)` → our reg hook fires
2. Kernel sees SanDisk device connected → matches LDD → calls `probe` → `attach`
3. Game calls `cellUsbdOpenPipe` → our GOT hook fires
4. Game calls `cellUsbdInterruptTransfer` → our GOT hook fires

Is this correct? Or does step 2 bypass our hooks entirely since probe/attach happen inside libusbd before reaching our GOT-level interception?

---

## Summary

We've shifted from trying to trigger the game's USB path externally (which failed because the kernel filter blocks non-matching devices) to patching the game itself to match a device we control. The EBOOT is patched, signed, and currently being FTPed to the PS3's HDD. Once the JB folder is in place, we can launch the game from HDD with the SanDisk VID/PID and test whether the kernel delivers our emulated device.

**The critical unknowns:** (1) Will the kernel deliver the device without additional checks? (2) Will probe/attach fire and succeed? (3) Can the bridge server properly emulate a SanDisk USB device?
