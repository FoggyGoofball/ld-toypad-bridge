#!/bin/bash
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"
git add -A
git commit -m "v8: COMPLETE SONY SDK PORT — ldtoypad SPRX builds with -mprx

BREAKING: Full migration from PSL1GHT to Sony DUPLEX SDK toolchain.

PSL1GHT APIs removed:
  - sysLv2FsOpen/sysFsWrite/sysFsClose → cellFsOpen/cellFsWrite/cellFsClose
  - sysNetSocket/sysNetBind/sysNetSendto/sysNetRecvfrom → socket/bind/sendto/recvfrom
  - sysUsbdInitialize/sysUsbdRegisterExtraLdd → cellUsbdInit/cellUsbdRegisterExtraLdd
  - sysUsleep → raw syscall 74 inline asm
  - <ppu-types.h> → <stdint.h>, <sys/types.h>

Sony SDK features added:
  - -mprx target with prx_crt0.o (no more .lib.stub collisions!)
  - cellSysmoduleLoadModule(CELL_SYSMODULE_FS/NET/USBD)
  - -lc_stub provides memcpy/memset/strlen/close
  - syscall.h: sys_usleep + sys_process_exit via inline asm

Build verified:
  - Compiler: CELL SDK host-win32 gcc.exe
  - Libraries: -llv2_stub -lfs_stub -lnet_stub -lsysmodule_stub -lusbd_stub -lc_stub
  - Post-link: make_self (scetool WSL workaround)
  - Output: build/ldtoypad.sprx (11,712 bytes, signed)
  - Critical ELF sections verified: .lib.ent, .lib.stub, .rodata.sceModule — NO COLLISIONS

Files: main.c debug.c network.c ldd_driver.c toypad_state.c Makefile
  syscall.h (new) compat.c debug.h ldd_driver.h network.h toypad_state.h
  ftp-deploy.ps1 ldbuild.sh docs/CHANGELOG-2026-07-18-v8-complete-port.md"
git push
