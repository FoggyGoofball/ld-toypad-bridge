#!/usr/bin/env python3
"""Find _sceModuleInfo structure in libusbd ELF by searching for module name."""
import sys, struct

def u16(data, off):
    return struct.unpack_from('>H', data, off)[0]
def u32(data, off):
    return struct.unpack_from('>I', data, off)[0]
def u64(data, off):
    return struct.unpack_from('>Q', data, off)[0]

with open(sys.argv[1], 'rb') as f:
    data = f.read()

for s in [b'libusbd', b'cellUsbd']:
    pos = 0
    while True:
        pos = data.find(s, pos)
        if pos == -1:
            break
        if pos >= 4:
            struct_start = pos - 4
            attr = u16(data, struct_start)
            ver = u16(data, struct_start + 2)
            gp = u64(data, struct_start + 32)
            ent_top = u64(data, struct_start + 40)
            ent_end = u64(data, struct_start + 48)
            
            if 0 < ent_top < 0xB8C0 and 0 < ent_end < 0xB8C0:
                nn = data[struct_start+4:struct_start+32].split(b'\x00')[0].decode()
                print(f"VALID _sceModuleInfo at file 0x{struct_start:X}")
                print(f"  modname: '{nn}'  attr=0x{attr:04X}  ver=0x{ver:04X}")
                print(f"  ent_top: vaddr 0x{ent_top:X} (file 0x{0xF0+ent_top:X})")
                print(f"  ent_end: vaddr 0x{ent_end:X} (file 0x{0xF0+ent_end:X})")
                
                exp_data = data[0xF0+ent_top:0xF0+ent_end]
                print(f"  Export table: {len(exp_data)} bytes")
                
                for es in [16, 24, 32]:
                    print(f"\n  --- {es}-byte stride ---")
                    off = 0
                    c = 0
                    while off + es <= len(exp_data) and c < 25:
                        vals = [u32(exp_data, off+i*4) for i in range(es//4)]
                        code = [v for v in vals if 0x100 <= v <= 0x9800]
                        fnid = [v for v in vals if v > 0x10000000]
                        if code or fnid:
                            vs = ' '.join(f'{v:08X}' for v in vals)
                            tag = ''
                            if code: tag += f' code=0x{code[0]:04X}'
                            if fnid: tag += f' fnid=0x{fnid[0]:08X}'
                            print(f"  [{c:2d}] {vs}{tag}")
                            c += 1
                        off += es
        pos += 1
