#!/usr/bin/env python3
"""
find_ldd_ops2.py — Find game's CellUsbdLddOps struct via ELF parsing.

The expert says: find the call site of cellUsbdRegisterLdd in the EBOOT,
read the lis+addi pair loading the struct address into r3.

Approach:
1. Parse ELF program headers to map VA → file offset
2. Find the .opd section containing the RegisterLdd OPD entry
3. Search for code that loads the struct address and calls RegisterLdd
"""

import struct
import sys

def read_u32(data, off):
    return struct.unpack('>I', data[off:off+4])[0]
def read_u16(data, off):
    return struct.unpack('>H', data[off:off+2])[0]

def va_to_file(va, segments):
    """Convert virtual address to file offset using ELF program headers."""
    for vaddr, filesz, offset in segments:
        if vaddr <= va < vaddr + filesz:
            return offset + (va - vaddr)
    return None

def main():
    elf_path = r"c:\Users\Admin\source\repos\dimensions plugin\EBOOT.elf"
    
    with open(elf_path, 'rb') as f:
        data = f.read()

    # Parse ELF header
    if data[:4] != b'\x7fELF':
        print("Not an ELF file")
        return
    
    is_64bit = data[4] == 2
    is_be = data[5] == 2  # 1=LE, 2=BE
    
    print(f"ELF: {'64' if is_64bit else '32'}-bit, {'BE' if is_be else 'LE'}")
    
    # Parse program headers
    if is_64bit:
        phoff = struct.unpack_from('>Q', data, 32)[0]
        phentsize = struct.unpack_from('>H', data, 54)[0]
        phnum = struct.unpack_from('>H', data, 56)[0]
    else:
        phoff = struct.unpack_from('>I', data, 28)[0]
        phentsize = struct.unpack_from('>H', data, 42)[0]
        phnum = struct.unpack_from('>H', data, 44)[0]
    
    print(f"Program headers: {phnum} entries at offset 0x{phoff:X}")
    
    segments = []
    for i in range(phnum):
        off = phoff + i * phentsize
        if is_64bit:
            p_type = struct.unpack_from('>I', data, off)[0]
            p_offset = struct.unpack_from('>Q', data, off + 8)[0]
            p_vaddr = struct.unpack_from('>Q', data, off + 16)[0]
            p_filesz = struct.unpack_from('>Q', data, off + 32)[0]
        else:
            p_type = struct.unpack_from('>I', data, off)[0]
            p_offset = struct.unpack_from('>I', data, off + 4)[0]
            p_vaddr = struct.unpack_from('>I', data, off + 8)[0]
            p_filesz = struct.unpack_from('>I', data, off + 16)[0]
        
        if p_type == 1:  # PT_LOAD
            segments.append((p_vaddr, p_filesz, p_offset))
            print(f"  LOAD: VA=0x{p_vaddr:08X} file=0x{p_offset:08X} size=0x{p_filesz:X}")
    
    # The game's RegisterLdd OPD is at VA 0x02C30060
    # Convert to file offset and read the OPD {func, toc}
    opd_va = 0x02C30060
    file_off = va_to_file(opd_va, segments)
    
    if file_off is None:
        print(f"\nCannot map VA 0x{opd_va:08X} to file offset")
        return
    
    func_va = read_u32(data, file_off)
    toc_va = read_u32(data, file_off + 4)
    print(f"\nRegisterLdd OPD at VA 0x{opd_va:08X} → file 0x{file_off:08X}")
    print(f"  func_addr = 0x{func_va:08X}")
    print(f"  toc_addr  = 0x{toc_va:08X}")
    
    # Now search for code that calls through this OPD.
    # Pattern: the code loads the struct address via lis+addi, then loads
    # the RegisterLdd OPD address (0x02C30060) from TOC, then branches.
    
    # Search the entire file for 0x02C30060 as a 32-bit big-endian value
    target_bytes = struct.pack('>I', opd_va)
    print(f"\nSearching for references to RegisterLdd OPD (0x{opd_va:08X})...")
    
    for i in range(0, len(data) - 3):
        if data[i:i+4] == target_bytes:
            # Found a reference. Look backward for lis/addi pattern
            ctx_start = max(0, i - 64)
            ctx = data[ctx_start:i]
            
            # Search backwards for lis+addi loading struct address
            struct_hi = None
            struct_lo = None
            for j in range(len(ctx) - 4, 0, -4):
                instr = read_u32(ctx, j)
                op = (instr >> 26) & 0x3F
                
                # lis r3, val (opcode 15, rD=3, rA=0)
                if op == 15 and ((instr >> 21) & 0x1F) == 3 and (instr & 0x00020000) == 0:
                    struct_hi = instr & 0xFFFF
                    struct_lo_offset = j + ctx_start + 4
                    # Check next instruction for addi r3, r3, val
                    if struct_lo_offset + 4 <= i:
                        next_instr = read_u32(data, struct_lo_offset)
                        next_op = (next_instr >> 26) & 0x3F
                        if next_op == 14 and ((next_instr >> 21) & 0x1F) == 3 and ((next_instr >> 16) & 0x1F) == 3:
                            struct_lo = next_instr & 0xFFFF
                            if struct_lo & 0x8000:
                                struct_lo -= 0x10000
                            break
            
            if struct_hi is not None and struct_lo is not None:
                struct_addr = (struct_hi << 16) | (struct_lo & 0xFFFF)
                print(f"\n  *** FOUND at file offset 0x{i:08X}:")
                print(f"      lis r3, 0x{struct_hi:04X}")
                print(f"      addi r3, r3, {struct_lo}")
                print(f"      → struct_addr = 0x{struct_addr:08X}")
                print(f"      → Runtime: 0x{struct_addr + 0x00010000:08X}")

if __name__ == '__main__':
    main()
