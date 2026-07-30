#!/usr/bin/env python3
"""
find_ldd_ops.py — Find the static address of the game's CellUsbdLddOps struct.

Strategy:
1. Load the decrypted EBOOT.elf
2. Find all references to cellUsbdRegisterLdd (OPD address 0x2C30060 in memory)
3. Trace back from each call site to find the struct address passed in r3
4. The struct is loaded via lis+addi (or lis+ori) pair

Since PS3 has no ASLR, the address we find here is the PERMANENT runtime address.
"""

import struct
import sys

def read_uint32(data, offset):
    return struct.unpack('>I', data[offset:offset+4])[0]

def read_uint16(data, offset):
    return struct.unpack('>H', data[offset:offset+2])[0]

def main():
    elf_path = r"c:\Users\Admin\source\repos\dimensions plugin\EBOOT.elf"
    
    try:
        with open(elf_path, 'rb') as f:
            data = f.read()
    except FileNotFoundError:
        print(f"ERROR: {elf_path} not found")
        print("Make sure EBOOT.elf is in the workspace root")
        return

    print(f"Loaded EBOOT.elf: {len(data)} bytes ({len(data)/(1024*1024):.1f} MB)")

    # The game's RegisterLdd OPD is at runtime address 0x2C30060
    # In the ELF, this would be at file_offset = runtime_addr - 0x00010000 + elf_offset
    # But we need to parse ELF headers to find the correct segment mapping.
    
    # Simpler approach: search for references to 0x2C30060 in the file
    # The OPD address itself appears as a 32-bit big-endian value
    
    target_opd_addr = 0x02C30060  # Game's cellUsbdRegisterLdd OPD
    
    print(f"\nSearching for references to game RegisterLdd OPD (0x{target_opd_addr:08X})...")
    
    matches = []
    for i in range(0, len(data) - 4, 4):
        val = read_uint32(data, i)
        if val == target_opd_addr:
            matches.append(i)
    
    print(f"Found {len(matches)} references in ELF file")
    
    # Show context around each match (searching for struct load patterns)
    for m in matches[:10]:
        # Show surrounding bytes as potential instructions
        start = max(0, m - 32)
        end = min(len(data), m + 32)
        chunk = data[start:end]
        
        print(f"\n  Match at file offset 0x{m:08X}:")
        # Look for lis/addi patterns in the preceding 32 bytes
        for j in range(max(0, m - 32), m, 4):
            instr = read_uint32(data, j)
            opcode = (instr >> 26) & 0x3F
            
            # lis = opcode 15 (addis rD, 0, SIMM)
            if opcode == 15 and (instr & 0x001F0000) == 0:  # rA = 0 for lis
                rd = (instr >> 21) & 0x1F
                simm = instr & 0xFFFF
                print(f"    lis r{rd}, 0x{simm:04X}  at file+0x{j:08X}")
            
            # addi = opcode 14
            if opcode == 14:
                rd = (instr >> 21) & 0x1F
                ra = (instr >> 16) & 0x1F
                simm = instr & 0xFFFF
                if simm & 0x8000: simm -= 0x10000
                print(f"    addi r{rd}, r{ra}, {simm}  at file+0x{j:08X}")
            
            # ori = opcode 24
            if opcode == 24:
                rd = (instr >> 21) & 0x1F
                ra = (instr >> 16) & 0x1F
                uimm = instr & 0xFFFF
                print(f"    ori r{rd}, r{ra}, 0x{uimm:04X}  at file+0x{j:08X}")

if __name__ == '__main__':
    main()
