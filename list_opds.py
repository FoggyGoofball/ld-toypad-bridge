#!/usr/bin/env python3
"""Enumerate all OPD entries in libusbd data section to find function exports."""
import struct, sys

def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def u64(d, o): return struct.unpack_from('>Q', d, o)[0]

with open(sys.argv[1], 'rb') as f:
    data = f.read()

# Data section: file offset 0x98F0, vaddr 0x9800, size 0x380
# OPD entries are 24 bytes each: code_addr(8), toc_addr(8), env_ptr(8)
# Scan data section for OPDs pointing to valid .text addresses

data_start = 0x98F0
data_end = 0x98F0 + 0x380

print("=== All OPD entries pointing to .text functions ===")
print(f"{'idx':>4s}  {'OPD_vaddr':>8s}  {'code_vaddr':>10s}  {'toc':>18s}  {'first_insn':>10s}")
print("-" * 65)

opds = []
for off in range(data_start, data_end, 24):
    opd_vaddr = off - 0xF0
    code = u64(data, off)
    toc = u64(data, off + 8)
    env = u64(data, off + 16)
    
    # Check if code_addr is a valid .text vaddr (0x0000-0x97FF)
    # For PPC64 PRX, code_addr in OPD is a 32-bit offset stored in the
    # lower 32 bits of the 64-bit field, or sometimes as a full 64-bit value
    code32_low = code & 0xFFFFFFFF
    code32_high = (code >> 32) & 0xFFFFFFFF
    
    code_vaddr = 0
    if 0 < code32_low < 0x9800:
        code_vaddr = code32_low
    elif 0 < code32_high < 0x9800:
        code_vaddr = code32_high
    elif 0 < code < 0x9800:
        code_vaddr = code
    
    if code_vaddr > 0:
        file_code = 0xF0 + code_vaddr
        first_insn = u32(data, file_code) if file_code + 4 <= len(data) else 0
        opds.append((opd_vaddr, code_vaddr, toc, first_insn))

# Sort by OPD vaddr
opds.sort(key=lambda x: x[0])

for i, (opd_va, code_va, toc, insn) in enumerate(opds):
    print(f"{i:4d}  0x{opd_va:04X}   0x{code_va:04X}       0x{toc:016X}  0x{insn:08X}")

print(f"\nTotal OPD entries: {len(opds)}")
print(f"\n=== Sorted by code address ===")
sorted_by_code = sorted(opds, key=lambda x: x[1])
for i, (opd_va, code_va, toc, insn) in enumerate(sorted_by_code):
    print(f"{i:4d}  OPD=0x{opd_va:04X}  code=0x{code_va:04X}  insn=0x{insn:08X}")
