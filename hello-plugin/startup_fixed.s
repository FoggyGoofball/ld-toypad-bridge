/*
 * startup_fixed.s -- Pure-assembly diagnostic SPRX for Cobra 8.5
 *
 * ZERO PSL1GHT DEPENDENCIES.
 * Provides its own _start, module_start, module_stop.
 * Writes /dev_hdd0/tmp/helloworld.txt via raw LV2 syscalls.
 *
 * section naming strategy:
 *   .rodata.sceModuleInfo -- survives because this is a .s file, not inline C asm
 *   .lib.ent -- export NID table (module_start NID 0xB109AFB0, module_stop NID 0x2A1DC125)
 *   .lib.stub -- empty import bracket (zero imports -- we use raw syscalls)
 *   No lv2-sprx.o linked -> no .lib.stub collision
 */

	.section .rodata.sceModuleInfo, "a", @progbits
	.p2align 2
__sce_module_info:
	.hword 0x0004		/* attr = SYS_PRX_RESIDENT (0x0004) */
	.hword 0x0101		/* version = 1.1 */
	.int 0			/* reserved */
	.asciz "hello_ldr"	/* module name (9 chars + NUL = 10 bytes) */
	.zero 18		/* pad name field to 28 bytes total */
	.int 0			/* reserved */
	.4byte __libentstart	/* libent start ptr (32-bit reloc) */
	.4byte __libentend	/* libent end ptr (32-bit reloc) */
	.4byte __libstubstart	/* libstub start ptr (32-bit reloc) */
	.4byte __libstubend	/* libstub end ptr (32-bit reloc) */
	.previous

	.section .lib.ent, "a", @progbits
	.p2align 2
	.globl __libentstart
__libentstart:
	.int 0xB109AFB0		/* NID: module_start */
	.int 0			/* reserved / padding */
	.8byte module_start	/* function pointer (64-bit addr) */
	.int 0x2A1DC125		/* NID: module_stop */
	.int 0			/* reserved / padding */
	.8byte module_stop	/* function pointer (64-bit addr) */
	.globl __libentend
__libentend:
	.previous

	.section .lib.stub, "a", @progbits
	.p2align 2
	.globl __libstubstart
__libstubstart:
	.globl __libstubend
__libstubend:
	.previous

	/* String constants in .rodata */
	.section .rodata.str, "a", @progbits
	.p2align 3
hello_path:
	.asciz "/dev_hdd0/tmp/helloworld.txt"
	.p2align 3
hello_msg:
	.asciz "Hello from SPRX loader!\n"

	.text
	.p2align 2

/*
 * _start -- PRX entry point
 * Must not assume r2 (TOC) is set up.  We use PC-relative addressing.
 */
	.globl _start
	.type _start, @function
_start:
	/* Save LR */
	mflr 0
	std 0, 16(1)
	stdu 1, -192(1)

	bl module_start

	/* Restore and return */
	addi 1, 1, 192
	ld 0, 16(1)
	mtlr 0
	blr
	.size _start, . - _start

/*
 * module_start -- writes hello-world file via raw LV2 syscalls
 *
 * Syscall 837: sysLv2FsOpen(path, oflags, &fd, mode, NULL, 0)
 * Syscall 839: sysFsWrite(fd, buf, len, &written, NULL, 0)
 * Syscall 838: sysFsClose(fd, NULL, 0, 0, 0, 0)
 *
 * All data accessed PC-relative (no TOC dependency).
 */
	.globl module_start
	.type module_start, @function
module_start:
	mflr 0
	std 0, 16(1)
	stdu 1, -192(1)

	/* Get current PC into r31 for data relative addressing */
	bl 0f
0:	mflr 31

	/* sysLv2FsOpen("/dev_hdd0/tmp/helloworld.txt", O_WRONLY|O_CREAT|O_TRUNC, &fd, 0666, NULL, 0) */
	addis 3, 31, (hello_path - 0b)@ha
	addi 3, 3, (hello_path - 0b)@l
	li 4, 0x0207		/* O_WRONLY | O_CREAT | O_TRUNC */
	addi 5, 1, 176		/* &fd on stack */
	li 6, 0666		/* mode */
	li 7, 0			/* NULL arg5 */
	li 8, 0			/* 0 arg6 */
	li 11, 837		/* sysLv2FsOpen */
	sc

	/* sysFsWrite(fd, msg, 25, &written, NULL, 0) */
	ld 3, 176(1)		/* fd from stack */
	addis 4, 31, (hello_msg - 0b)@ha
	addi 4, 4, (hello_msg - 0b)@l
	li 5, 25		/* length */
	addi 6, 1, 184		/* &written on stack */
	li 7, 0			/* NULL */
	li 8, 0			/* 0 */
	li 11, 839		/* sysFsWrite */
	sc

	/* sysFsClose(fd, NULL, 0, 0, 0, 0) */
	ld 3, 176(1)		/* fd from stack */
	li 4, 0
	li 5, 0
	li 6, 0
	li 7, 0
	li 8, 0
	li 11, 838		/* sysFsClose */
	sc

	/* Return SYS_PRX_RESIDENT (0) */
	li 3, 0

	addi 1, 1, 192
	ld 0, 16(1)
	mtlr 0
	blr
	.size module_start, . - module_start

/*
 * module_stop
 */
	.globl module_stop
	.type module_stop, @function
module_stop:
	li 3, 0
	blr
	.size module_stop, . - module_stop
