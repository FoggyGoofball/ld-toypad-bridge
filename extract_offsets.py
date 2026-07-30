#!/usr/bin/env python3
"""Parse libusbd.sprx (decrypted ELF) export table to find function offsets."""
import struct
import sys

def read_u32(data, off):
    return struct.unpack_from('>I', data, off)[0]

def read_u64(data, off):
    return struct.unpack_from('>Q', data, off)[0]

with open(sys.argv[1], 'rb') as f:
    elf = f.read()

# Verify ELF magic
assert elf[0:4] == b'\x7fELF', "Not an ELF file"

# Read program headers (64-bit, big-endian)
e_phoff = read_u64(elf, 0x20)
e_phnum = struct.unpack_from('>H', elf, 0x38)[0]
e_phentsize = struct.unpack_from('>H', elf, 0x36)[0]

print(f"Program headers at offset {e_phoff:#x}, count {e_phnum}, size {e_phentsize}")

segments = []
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type = read_u32(elf, off)
    p_flags = read_u32(elf, off + 4)
    p_offset = read_u64(elf, off + 8)
    p_vaddr = read_u64(elf, off + 16)
    p_paddr = read_u64(elf, off + 24)
    p_filesz = read_u64(elf, off + 32)
    p_memsz = read_u64(elf, off + 40)
    p_align = read_u64(elf, off + 48)
    segments.append({
        'type': p_type, 'flags': p_flags, 'offset': p_offset,
        'vaddr': p_vaddr, 'filesz': p_filesz, 'memsz': p_memsz
    })
    type_name = {1: 'LOAD', 0x700000a4: 'LOPROC+0xa4'}.get(p_type, f'0x{p_type:x}')
    flags_str = ''
    if p_flags & 4: flags_str += 'R'
    if p_flags & 2: flags_str += 'W'
    if p_flags & 1: flags_str += 'E'
    print(f"  [{i}] {type_name:12s} off={p_offset:#010x} vaddr={p_vaddr:#010x} "
          f"filesz={p_filesz:#x} memsz={p_memsz:#x} flags={flags_str}")

# Find the LOPROC segment (export metadata)
loproc = None
for s in segments:
    if s['type'] == 0x700000a4:
        loproc = s
        break

if not loproc:
    print("ERROR: No LOPROC segment found!")
    sys.exit(1)

print(f"\nLOPROC segment at file offset {loproc['offset']:#x}, size {loproc['filesz']:#x}")

# Parse the export table
# The SCE PRX export table format:
# Each library has a header followed by function entries
data = elf
off = loproc['offset']
end = off + loproc['filesz']

lib_count = 0
fn_count_total = 0

while off + 32 <= end:
    # Try to read as a library header
    lib_nid = read_u64(data, off)
    lib_ver = read_u32(data, off + 8)
    fn_count = read_u32(data, off + 12)
    
    # Check if this looks like a valid library header
    if fn_count == 0 or fn_count > 100:
        off += 4
        continue
    
    zero = read_u64(data, off + 16)
    if zero != 0:
        off += 4
        continue
    
    fn_nid_table = read_u64(data, off + 24)
    fn_ver2 = read_u32(data, off + 32)
    fn_count2 = read_u32(data, off + 36)
    
    if fn_count != fn_count2:
        off += 4
        continue
    
    zero2 = read_u64(data, off + 40)
    if zero2 != 0:
        off += 4
        continue
    
    fn_addr_table = read_u64(data, off + 48)
    
    lib_count += 1
    print(f"\nLibrary {lib_count}: NID=0x{lib_nid:016x} ver=0x{lib_ver:04x} "
          f"functions={fn_count}")
    print(f"  FNID table at 0x{fn_nid_table:x}, addr table at 0x{fn_addr_table:x}")
    
    # Read function entries
    for i in range(fn_count):
        fnid_off = fn_nid_table + i * 4
        addr_off = fn_addr_table + i * 4
        if fnid_off + 4 <= len(data) and addr_off + 4 <= len(data):
            fnid = read_u32(data, fnid_off)
            addr = read_u32(data, addr_off)
            print(f"  [{i:2d}] FNID=0x{fnid:08x}  addr=0x{addr:08x}  "
                  f"(file offset: 0x{0xf0 + addr:x})")
            fn_count_total += 1
    
    off += 64  # Library header is 64 bytes

print(f"\nTotal: {lib_count} libraries, {fn_count_total} functions")

# Now look for the specific cellUsbd functions we need
# Known NIDs from the SDK:
TARGET_NIDS = {
    0x7F5F00D3: 'cellUsbdOpenPipe',
    0x1AB6D80B: 'cellUsbdInterruptTransfer',  
    0x7B4436CE: 'cellUsbdClosePipe',
    0x2F82F1A5: 'cellUsbdGetDeviceDescriptor',
    # cellUsbdControlTransfer NID unknown - need to find
}

print("\n=== TARGET FUNCTION SEARCH ===")
off = loproc['offset']
while off + 32 <= end:
    lib_nid = read_u64(data, off)
    fn_count = read_u32(data, off + 12)
    zero = read_u64(data, off + 16)
    if fn_count == 0 or fn_count > 100 or zero != 0:
        off += 4
        continue
    fn_nid_table = read_u64(data, off + 24)
    fn_addr_table = read_u64(data, off + 48)
    
    for i in range(fn_count):
        fnid_off = fn_nid_table + i * 4
        addr_off = fn_addr_table + i * 4
        if fnid_off + 4 <= len(data) and addr_off + 4 <= len(data):
            fnid = read_u32(data, fnid_off)
            addr = read_u32(data, addr_off)
            if fnid in TARGET_NIDS:
                name = TARGET_NIDS[fnid]
                print(f"  FOUND: {name} -> 0x{addr:08X} (FNID=0x{fnid:08X})")
                print(f"    #define LIBUSBD_OFFSET_{name.upper()} 0x{addr:08X}")
    off += 64
