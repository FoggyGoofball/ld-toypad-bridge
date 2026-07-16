/** crt0.S
 * SPRX entry point and export table for LD-ToyPad Bridge.
 *
 * Provides:
 *   _start       - ELF entry with proper .opd 64-bit function descriptor
 *   .lib.ent     - exports module_start/module_stop
 *   .lib.stub    - empty section markers (sprxlinker fills this)
 *
 * The .sys_proc_prx_param is provided by the stock lv2-sprx.o which
 * ships with PSL1GHT.  It has the correct layout and magic (0x1B434CEC).
 * We just provide the symbols it references (__libentstart, etc.).
 */

.section .text, "ax"
.globl _start
.type  _start, @function
_start:
	mflr    %r0
	std     %r0, 16(%r1)
	bl      module_start
	nop
	ld      %r0, 16(%r1)
	mtlr    %r0
	blr
	.size  _start, . - _start

.section .lib.ent, "a"
.balign 4
.globl __libentstart
__libentstart:
	.byte   0x01
	.byte   0x00
	.4byte  0xB109AFB0
	.4byte  name_module_start - .lib.ent.str
	.balign 4
	.byte   0x01
	.byte   0x00
	.4byte  0x0B1C1CAA
	.4byte  name_module_stop - .lib.ent.str
	.balign 4
.globl __libentend
__libentend:

.section .lib.ent.str, "aMS", %progbits, 1
name_module_start:
	.asciz "module_start"
name_module_stop:
	.asciz "module_stop"

.section .lib.stub, "a"
.balign 4
.globl __libstubstart
.weak   __libstubstart
__libstubstart:
.globl __libstubend
.weak   __libstubend
__libstubend:
