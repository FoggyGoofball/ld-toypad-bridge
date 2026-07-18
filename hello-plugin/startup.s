/*
 * startup.s -- Minimal "Hello World" SPRX for Cobra 8.5 (Evilnat CFW)
 *
 * Pure assembly with zero external dependencies.
 * No TOC pointer needed -- uses PC-relative addressing for all data.
 *
 * Contains:
 *   .rodata.sceModuleInfo  -- mandatory PRX identity section
 *   .lib.ent                -- export NID table (module_start, module_stop)
 *   .lib.stub               -- empty import bracket
 *   _start                  -- entry point (calls module_start)
 *   module_start            -- writes /dev_hdd0/plugins/helloworld.txt
 *   module_stop             -- stub
 */

.section .rodata.sceModuleInfo, "a", @progbits
__sce_module_info:
 .hword 0x0004
 .hword 0x0101
 .int 0
 .asciz "hello_loader"
 .zero 16
 .int 0
 .4byte __libentstart
 .4byte __libentend
 .4byte __libstubstart
 .4byte __libstubend
.previous

.section .lib.ent, "a", @progbits
.globl __libentstart
__libentstart:
 .int 0xB109AFB0
 .int 0
 .8byte module_start
 .int 0x2A1DC125
 .int 0
 .8byte module_stop
.globl __libentend
__libentend:
.previous

.section .lib.stub, "a", @progbits
.globl __libstubstart
__libstubstart:
.globl __libstubend
__libstubend:
.previous

.section .rodata.str, "a", @progbits
.balign 8
hello_file_str:
 .asciz "/dev_hdd0/plugins/helloworld.txt"
.balign 8
hello_msg_str:
 .asciz "Hello from SPRX loader!\n"
.text

/*
 * _start entry point.
 * We don't set up any TOC -- we use PC-relative addressing.
 */
.globl _start
.type _start, @function
_start:
 bl module_start
 blr

/*
 * module_start -- writes hello-world file via raw LV2 syscalls
 *
 * Syscall 837: sysLv2FsOpen(path, oflags, &fd, mode, 0, 0)
 * Syscall 839: sysFsWrite(fd, buf, 25, &written, 0, 0)
 * Syscall 838: sysFsClose(fd, 0, 0, 0, 0, 0)
 */
.globl module_start
.type module_start, @function
module_start:
 mflr 0
 std 0, 16(1)
 stdu 1, -192(1)

 /* Get current PC into r31 for data relative addressing */
 bl 0f
0:
 mflr 31

 /* sysLv2FsOpen */
 addis 3, 31, (hello_file_str - 0b)@ha
 addi 3, 3, (hello_file_str - 0b)@l
 li 4, 0x0209
 addi 5, 1, 176
 li 6, 0666
 li 7, 0
 li 8, 0
 li 11, 837
 sc

 /* sysFsWrite */
 ld 3, 176(1)
 addis 4, 31, (hello_msg_str - 0b)@ha
 addi 4, 4, (hello_msg_str - 0b)@l
 li 5, 25
 addi 6, 1, 184
 li 7, 0
 li 8, 0
 li 11, 839
 sc

 /* sysFsClose */
 ld 3, 176(1)
 li 4, 0
 li 5, 0
 li 6, 0
 li 7, 0
 li 8, 0
 li 11, 838
 sc

 /* Return SYS_PRX_RESIDENT (0) */
 li 3, 0

 addi 1, 1, 192
 ld 0, 16(1)
 mtlr 0
 blr
.size module_start, . - module_start

.globl module_stop
.type module_stop, @function
module_stop:
 li 3, 0
 blr
.size module_stop, . - module_stop
