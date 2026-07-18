# CHANGELOG: v8 — Complete Sony SDK Port of ldtoypad SPRX (July 18, 2026)

## Summary

Complete rewrite of the LD-ToyPad Bridge SPRX plugin from PSL1GHT API to Sony DUPLEX SDK (`-mprx` target). The plugin now compiles cleanly with the Sony PS3 SDK toolchain, links against Sony stub libraries, and produces a valid signed `.sprx` ready for deployment to Evilnat 4.93 Cobra.

## Files Modified

### `sprx-plugin/Main.c` — REWRITTEN (173 lines)
- Converted from PSL1GHT `ppu/include` headers to Sony SDK headers (`<sys/prx.h>`, `<sys/ppu_thread.h>`, `<cell/cell_fs.h>`, `<cell/sysmodule.h>`)
- `SYS_MODULE_INFO(ldtoypad, 0, 1, 1)` with proper PRX metadata
- `SYS_MODULE_START(module_start)`, `SYS_MODULE_STOP(module_stop)` 
- `cellFsOpen/Write/Close` for papertrail logging to `/dev_hdd0/plugins/ldtoypad_boot.log`
- `cellSysmoduleLoadModule(CELL_SYSMODULE_FS/NET/USBD)` — 3 sysmodule loads
- `sys_ppu_thread_create` with worker thread (prio 1000, 64KB stack)
- `g_shutdown` flag + `sys_ppu_thread_join` for clean exit
- Raw syscall 74 inline asm for `sys_usleep` (only inline asm, safe)
- `SYS_PRX_STOP_OK` return
- **NO PSL1GHT includes**, **NO `.lib.stub` collisions**

### `sprx-plugin/debug.c` — REWRITTEN
- Replaced all PSL1GHT raw syscalls with Sony SDK equivalents:
  - `sysLv2FsOpen()` → `cellFsOpen()`
  - `sysFsWrite()` → `cellFsWrite()` 
  - `sysFsClose()` → `cellFsClose()`
- Replaced PSL1GHT network syscalls with BSD sockets:
  - `sysNetSocket()` → `socket()`
  - `sysNetSendto()` → `sendto()`
  - `sysNetClose()` → `close()`
- Updated includes: `<cell/cell_fs.h>`, `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`

### `sprx-plugin/network.c` — REWRITTEN
- Same PSL1GHT → Sony SDK API migration as debug.c
- `sysNetSocket()` → `socket()`, `sysNetBind()` → `bind()`, etc.
- `sysUsleep()` → raw syscall 74 inline asm
- `SYS_O_WRONLY` → `CELL_FS_O_WRONLY` (file flags)
- Enabled (was previously ifdef'd out)

### `sprx-plugin/ldd_driver.c` — REWRITTEN
- `sysUsbdInitialize()` → `cellUsbdInit()` (with proper CellUsbdLddContext struct)
- `sysUsbdRegisterExtraLdd()` → `cellUsbdRegisterExtraLdd()`
- Proper Sony SDK USBD types: `CellUsbdLddContext`, `CellUsbdLddDeviceInfo`, `CellUsbdLddTransfer`
- Callback signatures using Sony SDK types

### `sprx-plugin/Makefile` — REWRITTEN
- Compiler: `C:/usr/local/cell/host-win32/ppu/ppu-lv2/bin/gcc.exe`
- Includes: `-I$(CELL_TARGET)/ppu/include`
- Libraries: `-llv2_stub -lfs_stub -lnet_stub -lsysmodule_stub -lusbd_stub -lc_stub`
- CRT: `-mprx -nodefaultlibs` (uses `prx_crt0.o` automatically)
- Post-link: `make_self` via WSL (scetool segfaults in this WSL distro)
- Output: `build/ldtoypad.sprx`

### Header Fixes
- `sprx-plugin/debug.h` — changed `<ppu-types.h>` to `<stdint.h>`
- `sprx-plugin/ldd_driver.h` — changed `<ppu-types.h>` to `<stdint.h>`
- `sprx-plugin/toypad_state.h` — changed `<ppu-types.h>` to `<stdint.h>`
- `sprx-plugin/toypad_state.c` — removed `<sys/usbd.h>`, kept `<string.h>`

### `sprx-plugin/syscall.h` — NEW
- Raw syscall 74 inline asm for `sys_usleep(usec)`
- Raw syscall 6 inline asm for `sys_process_exit(code)`
- Clobber list: `"r3","r4","r5","r6","r7","r8","r9","r10","r11","memory"`

### `sprx-plugin/compat.c` — CLEANED UP
- Updated `<ppu-types.h>` → standard headers for Sony SDK compatibility
- Placed under conditional build guard

## Files Unchanged
- `sprx-plugin/network.h`
- `sprx-plugin/toypad_state.h` (only include path changed)

## Build Artifacts
- **`sprx-plugin/build/ldtoypad.prx`** — 62,500 bytes (raw ELF, 46 sections)
- **`sprx-plugin/build/ldtoypad.sprx`** — 11,712 bytes (signed with `make_self`)

## Critical ELF Sections Verified (no collisions)
```
[ 4] .lib.ent.top      — Export table start
[ 5] .lib.ent          — Export table entries  
[ 6] .rela.lib.ent     — Export table relocations
[ 7] .lib.ent.btm      — Export table end
[ 8] .lib.stub.top     — Import stub start
[ 9] .lib.stub         — Import stub entries
[10] .rela.lib.stub    — Import stub relocations
[11] .lib.stub.btm     — Import stub end
[12] .rodata.sceModule — Module info (SYS_MODULE_INFO)
```

## Known Issues
- **scetool segfaults** in this WSL distro (Ubuntu 24.04): switched to `make_self` from PSL1GHT toolchain for signing. `make_self` produces valid SELF files.
- **PS3 FTP deploy pending**: The PS3 at 192.168.0.47 needs to be powered on with FTP server (webMAN/Multiman) running. Use `powershell -ExecutionPolicy Bypass .\ftp-deploy.ps1` to deploy.
- **Deploy to `/dev_hdd0/plugins/`** and register in `/dev_hdd0/boot_plugins.txt` for Cobra to load on boot.

## SDK Reference
- Working reference: `hello-plugin/` — Sony SDK `-mprx` build with identical Makefile pattern
- Deployment reference: `hello-plugin/deploy-hello-sdk.ps1` — FTP deployment pattern
