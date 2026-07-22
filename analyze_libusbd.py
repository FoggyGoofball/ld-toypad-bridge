import struct

with open('libusbd.sprx', 'rb') as f:
    data = f.read()

print(f'Total size: {len(data)} bytes')
print(f'Magic: {data[0:4].hex()}')

# ELF header parsing
if data[0:4] == b'\x7fELF':
    print("Valid ELF header found\n")
    
    e_type = struct.unpack('<H', data[16:18])[0]
    e_machine = struct.unpack('<H', data[18:20])[0]
    e_entry = struct.unpack('<Q', data[24:32])[0]
    e_phoff = struct.unpack('<Q', data[32:40])[0]
    e_shoff = struct.unpack('<Q', data[40:48])[0]
    e_flags = struct.unpack('<I', data[48:52])[0]
    e_ehsize = struct.unpack('<H', data[52:54])[0]
    e_phentsize = struct.unpack('<H', data[54:56])[0]
    e_phnum = struct.unpack('<H', data[56:58])[0]
    e_shentsize = struct.unpack('<H', data[58:60])[0]
    e_shnum = struct.unpack('<H', data[60:62])[0]
    e_shstrndx = struct.unpack('<H', data[62:64])[0]
    
    print(f'Type: {e_type}, Machine: 0x{e_machine:x} (PPC=20), Entry: 0x{e_entry:x}')
    print(f'PH off: 0x{e_phoff:x}, PH num: {e_phnum}')
    print(f'SH off: 0x{e_shoff:x}, SH num: {e_shnum}')
    print(f'Section strtab index: {e_shstrndx}')
    
    # Section string table
    shstrtab_off = e_shoff + e_shstrndx * e_shentsize
    fields = struct.unpack('<IIQQQQIIQQ', data[shstrtab_off:shstrtab_off+64])
    sh_name_str, _, _, _, sh_offset, sh_size = fields[:6]
    strtab = data[sh_offset:sh_offset+sh_size]
    
    print(f'\n=== All Sections ===')
    for i in range(e_shnum):
        sh_off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack('<IIQQQQIIQQ', data[sh_off:sh_off+64])
        name = strtab[sh_name:strtab.index(b'\x00', sh_name)].decode('utf-8', errors='replace') if sh_name < len(strtab) else '?'
        print(f'  [{i:2}] {name:25s} type=0x{sh_type:08x} addr=0x{sh_addr:016x} offset=0x{sh_offset:06x} size=0x{sh_size:06x}')
        
        if sh_size > 0:
            seg_data = data[sh_offset:sh_offset+sh_size]
            if sh_type == 0x70000020:  # SHT_PRX_LIB_ENTRY
                print(f'      *** PRX ENTRY SECTION ***')
                print(f'      Raw: {seg_data.hex()}')
                
            if 'lib.ent' in name or sh_type == 0x70000020 or 'lib.stub' in name:
                print(f'      Raw hex: {seg_data[:128].hex()}')

# Search for NID patterns in the file
print(f'\n=== Searching for known NID triplets ===')
known_ids = {
    'A75B4B8B6C689C5D': 'cellUsbdInit', 
    '3B1D23B74C26B3C4': 'cellUsbdEnd',
    'D9B779E6': 'cellUsbdOpenDevice_top',
    '4D1310DF': 'cellUsbdTransfer_top',
    'D99EA679': 'cellUsbdCloseDevice_top',
    '2CE4363D': 'cellUsbdCloseDevice_bot',
}

# Search for 8-byte sequences
for off in range(len(data) - 16):
    chunk = data[off:off+8]
    chunk_hex = chunk.hex().upper()
    if chunk_hex in known_ids:
        print(f'  Found {known_ids[chunk_hex]} ({chunk_hex}) at offset 0x{off:x}')
        # Show 24 bytes of context
        ctx = data[off:off+24].hex()
        print(f'  Context: {ctx}')

# Also search for the library name string
lib_name_offset = data.find(b'cellUsbd')
if lib_name_offset >= 0:
    print(f'\nFound "cellUsbd" string at offset 0x{lib_name_offset:x}')
    print(f'  Context: {data[lib_name_offset-8:lib_name_offset+16].hex()}')

# Search for "libusbd" 
lib_name_offset2 = data.find(b'libusbd')
if lib_name_offset2 >= 0:
    print(f'Found "libusbd" string at offset 0x{lib_name_offset2:x}')

# Dump the .lib.ent section specifically if not found via sections
# The lib entries in SPRX are at sh_type 0x70000020
print(f'\n=== Dumping all PRX-relevant sections (0x70000020 type) ===')
for i in range(e_shnum):
    sh_off = e_shoff + i * e_shentsize
    sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack('<IIQQQQIIQQ', data[sh_off:sh_off+64])
    if sh_type in (0x70000020, 0x70000022):  # SHT_PRX_LIB_ENTRY or DYNAMIC
        name = strtab[sh_name:strtab.index(b'\x00', sh_name)].decode('utf-8', errors='replace') if sh_name < len(strtab) else '?'
        print(f'\n--- Section [{i}] {name} type=0x{sh_type:x} ---')
        seg_data = data[sh_offset:sh_offset+sh_size]
        print(f'  Size: {sh_size} bytes')
        # Dump in a readable format
        for j in range(0, min(sh_size, 1024), 16):
            line = seg_data[j:j+16]
            hex_str = ' '.join(f'{b:02x}' for b in line)
            print(f'  +0x{j:04x}: {hex_str}')
        if sh_size > 1024:
            print(f'  ... truncated, total {sh_size} bytes')

# Also dump dynamic section
print(f'\n=== Dynamic Sections (type .dynamic or SHT_DYNAMIC=6) ===')
for i in range(e_shnum):
    sh_off = e_shoff + i * e_shentsize
    sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack('<IIQQQQIIQQ', data[sh_off:sh_off+64])
    if sh_type == 6:  # SHT_DYNAMIC
        name = strtab[sh_name:strtab.index(b'\x00', sh_name)].decode('utf-8', errors='replace') if sh_name < len(strtab) else '?'
        print(f'\n--- Section [{i}] {name} (SHT_DYNAMIC) ---')
        print(f'  addr=0x{sh_addr:x} offset=0x{sh_offset:x} size={sh_size}')
        # Parse dynamic entries
        seg_data = data[sh_offset:sh_offset+sh_size]
        for j in range(0, sh_size, 16):
            d_tag = struct.unpack('<Q', seg_data[j:j+8])[0]
            d_val = struct.unpack('<Q', seg_data[j+8:j+16])[0]
            print(f'  +0x{j:04x}: DT_TAG=0x{d_tag:x} D_VAL=0x{d_val:x}')
            if d_tag == 0:
                break
