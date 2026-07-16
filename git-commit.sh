#!/bin/bash
cd /mnt/c/Users/Admin/source/repos/dimensions\ plugin
git add -A
git commit -m "v6: Remove crt0.S, inject native PSL1GHT PRX macros

Checklist remediation:

1. DELETED crt0.S and write_crt0.py - custom CRT assembly removed
2. Removed S_SRCS/S_OBJS/-nostartfiles from Makefile
3. Added SYS_MODULE_INFO/START/STOP macros in main.c for
   native PRX metadata generation via <sys/prx.h>
4. Confirmed module_start/module_stop naming (already correct)
5. Preserved lv2-sprx.o linkage for _start and .sys_proc_prx_param

Architecture change: CRT chain now uses stock lv2-sprx.o for
_start + .sys_proc_prx_param, and PRX macros for module metadata
and export table generation. No custom crt0.S needed."
git push
