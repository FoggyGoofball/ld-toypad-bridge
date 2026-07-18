import os

asm = """/*
 * startup.S -- Minimal Hello World SPRX for Cobra 8.5
 * Pure assembly, no PSL1GHT dependencies.
 */
#include <ppu_asm.h>

.section .rodata.sceModuleInfo, "a", @progbits
__sce_module_info:
\t.hword 0x0004
\t.hword 0x0101
\t.int 0
\t.asciz "hello_loader"
\t.zero 16
\t.int 0
\t.4byte __libentstart
\t.4byte __libentend
\t.4byte __libstubstart
\t.4byte __libstubend
.previous

.section .lib.ent, "a", @progbits
.globl __libentstart
__libentstart:
\t.int 0xB109AFB0
\t.int 0
\t.8byte module_start
\t.int 0x2A1DC125
\t.int 0
\t.8byte module_stop
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
\t.asciz "/dev_hdd0/plugins/helloworld.txt"
.balign 8
hello_msg_str:
\t.asciz "Hello from SPRX loader!\\n"
.text

.section .toc, "aw"
hello_file_toc:
\t.tc hello_file_str[TC], hello_file_str
hello_msg_toc:
\t.tc hello_msg_str[TC], hello_msg_str
.text

.globl _start
.type _start, @function
_start:
\tmflr 0
\tbl _start_toc_get
_start_toc_get:
\tmflr 2
\taddis 2, 2, (_start_toc - _start_toc_get)@ha
\taddi 2, 2, (_start_toc - _start_toc_get)@l
\tld 2, 0(2)
\tmtlr 0
\tbl module_start
\tblr

.section .toc, "aw"
_start_toc:
\t.tc .TOC.[TC], .TOC.
.text

.globl module_start
.type module_start, @function
module_start:
\tmflr 0
\tstd 0, 16(1)
\tstdu 1, -192(1)
\tld 3, hello_file_toc@toc(2)
\tli 4, 0x0209
\taddi 5, 1, 176
\tli 6, 0666
\tli 7, 0
\tli 8, 0
\tli 11, 837
\tsc
\tld 3, 176(1)
\tld 4, hello_msg_toc@toc(2)
\tli 5, 25
\taddi 6, 1, 184
\tli 7, 0
\tli 8, 0
\tli 11, 839
\tsc
\tld 3, 176(1)
\tli 4, 0
\tli 5, 0
\tli 6, 0
\tli 7, 0
\tli 8, 0
\tli 11, 838
\tsc
\tli 3, 0
\taddi 1, 1, 192
\tld 0, 16(1)
\tmtlr 0
\tblr
.size module_start, . - module_start

.globl module_stop
.type module_stop, @function
module_stop:
\tli 3, 0
\tblr
.size module_stop, . - module_stop
"""

os.makedirs('/tmp/helloworld-build', exist_ok=True)
with open('/tmp/helloworld-build/startup.S', 'w') as f:
    f.write(asm)
print('Wrote startup.S successfully')
