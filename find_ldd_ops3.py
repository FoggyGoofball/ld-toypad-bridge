#!/usr/bin/env python3
"""Search EBOOT.elf LOAD segments for LDD name string and ops struct."""

import struct

def read_u32(data, off):
    return struct.unpack('>I', data[off:off+4])[0]

with open(r"c:\Users\Admin\source\repos\dimensions plugin\EBOOT.elf", 'rb') as f:
    data = f.read()

# LOAD segment 1: file=0x00000000, VA=0x00010000, size=0x1B2EDE8
# LOAD segment 2: file=0x01B30000, VA=0x01B40000, size=0x144074
seg1 = data[0x00000000:0x01B2EDE8]

# Search for strings that might be LDD names
# The CellUsbdLddOps struct starts with a string pointer
search_terms = [b'ldtoypad', b'ToyPad', b'toypad', b'Toy Pad', b'TOYPAD',
                b'Lego', b'LEGO', b'dimensions', b'portal', b'Portal',
                b'pad', b'Pad']

print("Searching for LDD name strings in EBOOT...")
for term in search_terms:
    pos = 0
    while True:
        pos = seg1.find(term, pos)
        if pos == -1: break
        va = 0x00010000 + pos
        print(f"  Found '{term.decode()}' at file+0x{pos:08X} (VA 0x{va:08X})")
        pos += 1

# Also search for struct pattern: {string_ptr, func_ptr, func_ptr, func_ptr}
# where all three func_ptrs point to valid OPD addresses
# OPD func_ptrs for this game are in the 0x01xxxxxx-0x02xxxxxx range
print("\nSearching for ops struct pattern in data section...")
# Look in segment 2 (data section at VA 0x01B40000)
seg2 = data[0x01B30000:0x01B30000+0x144074]

for off in range(0, len(seg2) - 16, 8):
    vals = [read_u32(seg2, off+i) for i in range(0, 16, 4)]
    
    # vals[0] = string pointer, vals[1..3] = function pointers
    # Check if [1..3] look like valid function pointers
    if all(0x00010000 < v < 0x03000000 for v in vals[1:4]):
        # Check if string pointer points to printable ASCII
        str_ptr = vals[0]
        str_file = str_ptr - 0x01B40000 + 0x01B30000  # VA → file offset
        str_file2 = str_ptr - 0x00010000  # If in segment 1
        
        # Only print if the name is at least 2 chars and printable
        if 0 < str_file2 < len(data) - 4:
            name_bytes = data[str_file2:str_file2+16]
            if all(0x20 <= b < 0x7f for b in name_bytes[:4]):
                name = name_bytes.split(b'\x00')[0].decode('ascii', errors='ignore')
                if len(name) >= 2:
                    va = 0x01B40000 + off
                    print(f"  Possible ops at VA 0x{va:08X}: name='{name}' ptrs=[0x{vals[1]:08X},0x{vals[2]:08X},0x{vals[3]:08X}]")

print("\nDone.")
