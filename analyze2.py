import struct, sys

data = open('libusbd.sprx','rb').read()
print('Total:', len(data))
sys.stdout.flush()

off = data.find(b'\x7fELF')
print('ELF at:', hex(off))
sys.stdout.flush()

if off < 0:
    exit()

elf = data[off:]
e_phoff = struct.unpack('<Q', elf[32:40])[0]
e_phentsize = struct.unpack('<H', elf[54:56])[0]
e_phnum = struct.unpack('<H', elf[56:58])[0]
print(f'PH off: 0x{e_phoff:x}, entsize: {e_phentsize}, num: {e_phnum}')

# Parse program headers
for i in range(e_phnum):
    p_off = e_phoff + i * e_phentsize
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack('<IIQQQQQQ', elf[p_off:p_off+56])
    print(f'  PH[{i}] type=0x{p_type:08x} flags=0x{p_flags:x} offset=0x{p_offset:x} vaddr=0x{p_vaddr:x} filesz=0x{p_filesz:x} memsz=0x{p_memsz:x}')

# Look for PRX export/import sections in program headers
# PT_PRX_LIB_ENTRY = 0x700000A0 (export), PT_PRX_LIB_STUB = 0x700000A1 (import)
print('\n=== PRX-specific program headers ===')
for i in range(e_phnum):
    p_off = e_phoff + i * e_phentsize
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack('<IIQQQQQQ', elf[p_off:p_off+56])
    if p_type in (0x700000A0, 0x700000A1, 0x700000A2):
        print(f'  PH[{i}] type=0x{p_type:x} vaddr=0x{p_vaddr:x} offset=0x{p_offset:x} filesz=0x{p_filesz:x}')
        abs_off = off + p_offset
        seg_data = data[abs_off:abs_off+p_filesz]
        print(f'  Raw data ({p_filesz} bytes):')
        for j in range(0, min(p_filesz, 512), 16):
            line = seg_data[j:j+16]
            h = ' '.join(f'{b:02x}' for b in line)
            a = ''.join(chr(b) if 32 <= b < 127 else '.' for b in line)
            print(f'    +{j:04x}: {h:48s} {a}')
        sys.stdout.flush()

# Also search for the NID triplets by scanning raw bytes
# The handoff doc says the OLD scanner was looking for:
# A75B4B8B6C689C5D (cellUsbdInit 64-bit)
# DA871DDB  (32-bit version/hash)
# Let me look for any 8-byte sequence that matches the known pattern in the raw data
print('\n=== Searching raw data for known function NIDs ===')
known_nids_8 = {
    'A75B4B8B6C689C5D': 'cellUsbdInit',
    '3B1D23B74C26B3C4': 'cellUsbdEnd',
}
known_nids_4 = ['DA871DDB', 'D9BBC3D2', '4D1310DF', '5F0DB93B', '2CE4363D']
# Also look for partial matches of the full triplets
known_triplets = [
    'A75B4B8B6C689C5DDA871DDB',
    '3B1D23B74C26B3C4D9BBC3D2',
]

for needle, name in known_nids_8.items():
    ba = bytes.fromhex(needle)
    pos = data.find(ba)
    if pos >= 0:
        print(f'  Found {name} ({needle}) at file offset 0x{pos:x}')
        ctx = data[max(0,pos-8):pos+24]
        print(f'  Context: {ctx.hex()}')

for needle in known_nids_4:
    ba = bytes.fromhex(needle)
    pos = data.find(ba)
    if pos >= 0:
        print(f'  Found 4-byte NID {needle} at file offset 0x{pos:x}')
        ctx = data[max(0,pos-8):pos+16]
        print(f'  Context: {ctx.hex()}')

for needle in known_triplets:
    ba = bytes.fromhex(needle)
    pos = data.find(ba)
    if pos >= 0:
        print(f'  Found FULL TRIPLET {needle[:16]}... at file offset 0x{pos:x}')
        ctx = data[max(0,pos-8):pos+32]
        print(f'  Context: {ctx.hex()}')
