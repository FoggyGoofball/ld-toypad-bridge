#!/usr/bin/env python3
"""ELF parser for LEGO Dimensions game ELF.
Searches for cellUsbd import stubs.
Usage: python3 dump_elf.py <path_to_game_elf>
"""

import sys
import struct

def parse_elf(path):
    with open(path, 'rb') as f:
        data = f.read()
    
    off = data.find(b'\x7fELF')
    if off < 0:
        print("ERROR: No ELF header found")
        return
    print(f"ELF at offset {off}, total size {len(data)}")
    
    elf = data[off:]
    
    # Check endianness
    ei_data = elf[5]  # 1=LE, 2=BE
    endian = '<' if ei_data == 1 else '>'
    print(f"Endianness: {'LE' if ei_data == 1 else 'BE'}")
    
    e_phoff = struct.unpack(endian + 'Q', elf[32:40])[0]
    e_phnum = struct.unpack(endian + 'H', elf[56:58])[0]
    e_phentsize = struct.unpack(endian + 'H', elf[54:56])[0]
    print(f"Program headers: {e_phnum} at 0x{e_phoff:x}, entry size {e_phentsize}")
    
    # Parse program headers
    for i in range(e_phnum):
        p_off = e_phoff + i * e_phentsize
        ph = elf[p_off:p_off+e_phentsize]
        p_type = struct.unpack(endian + 'I', ph[0:4])[0]
        p_flags = struct.unpack(endian + 'I', ph[4:8])[0]
        p_offset = struct.unpack(endian + 'Q', ph[8:16])[0]
        p_vaddr = struct.unpack(endian + 'Q', ph[16:24])[0]
        p_paddr = struct.unpack(endian + 'Q', ph[24:32])[0]
        p_filesz = struct.unpack(endian + 'Q', ph[32:40])[0]
        p_memsz = struct.unpack(endian + 'Q', ph[40:48])[0]
        p_align = struct.unpack(endian + 'Q', ph[48:56])[0]
        type_name = {
            1: 'PT_LOAD',
            0x700000A0: 'PT_PRX_LIB_ENTRY',
            0x700000A1: 'PT_PRX_LIB_STUB',
            0x700000A4: 'PT_PRX_RELOC'
        }.get(p_type, f'UNKNOWN(0x{p_type:x})')
        print(f"  PH[{i}] {type_name} flags=0x{p_flags:x} fileoff=0x{p_offset:x} vaddr=0x{p_vaddr:x} fsz=0x{p_filesz:x} msz=0x{p_memsz:x}")
    
    # Parse section headers
    e_shoff = struct.unpack(endian + 'Q', elf[40:48])[0]
    e_shnum = struct.unpack(endian + 'H', elf[60:62])[0]
    e_shentsize = struct.unpack(endian + 'H', elf[58:60])[0]
    e_shstrndx = struct.unpack(endian + 'H', elf[62:64])[0]
    
    print(f"\nSection headers: {e_shnum} at 0x{e_shoff:x}, entry size {e_shentsize}, string table idx {e_shstrndx}")
    
    if e_shnum == 0:
        print("No section headers (stripped ELF)")
    else:
        # Get section name string table
        shstr_off = e_shoff + e_shstrndx * e_shentsize
        shstr = elf[shstr_off:shstr_off+e_shentsize]
        shstr_offset = struct.unpack(endian + 'Q', shstr[24:32])[0]
        shstr_size = struct.unpack(endian + 'Q', shstr[32:40])[0]
        strtab = elf[shstr_offset:shstr_offset+shstr_size]
        
        for i in range(e_shnum):
            s_off = e_shoff + i * e_shentsize
            sh = elf[s_off:s_off+e_shentsize]
            sh_name = struct.unpack(endian + 'I', sh[0:4])[0]
            sh_type = struct.unpack(endian + 'I', sh[4:8])[0]
            sh_addr = struct.unpack(endian + 'Q', sh[16:24])[0]
            sh_offset = struct.unpack(endian + 'Q', sh[24:32])[0]
            sh_size = struct.unpack(endian + 'Q', sh[32:40])[0]
            
            name = strtab[sh_name:].split(b'\x00')[0].decode('ascii', errors='replace')
            if sh_size > 0:
                print(f"  SH[{i}] {name:25s} addr=0x{sh_addr:08x} offset=0x{sh_offset:08x} size=0x{sh_size:x}")
    
    # Search for import stubs in .text / LOAD segments
    # Pattern: 12-byte structure: NID(4) + reserved(4) + GOT_ptr(4)
    # CellOS import stubs
    print("\n=== Scanning for NID import stubs ===")
    # Common cellUsbd function NIDs (from known PS3 SDK)
    # These are 16-bit NIDs in the lis/ori instruction encoding
    # But the scanner looks for 32-bit NID triplets
    
    # CellOS system PRX functions use FNID (function NID) - 16-bit or 32-bit hash
    # Let's search for patterns that look like import stubs
    nids_to_try = [
        (b'\xD3\x00\x5F\x7F', 'cellUsbdInit 0x7F5F00D3'),
        (b'\x0B\xD8\xB6\x1A', 'cellUsbdOpenPipe 0x1AB6D80B'),
        (b'\xCE\x36\x44\x7B', 'cellUsbdTransfer 0x7B4436CE'),
        (b'\xA5\xF1\x82\x2F', 'cellUsbdClosePipe 0x2F82F1A5'),
        # Try reversed (big-endian PS3 uses BE)
        (b'\x7F\x5F\x00\xD3', 'cellUsbdInit 0x7F5F00D3 BE'),
        (b'\x1A\xB6\xD8\x0B', 'cellUsbdOpenPipe 0x1AB6D80B BE'),
        (b'\x7B\x44\x36\xCE', 'cellUsbdTransfer 0x7B4436CE BE'),
        (b'\x2F\x82\xF1\xA5', 'cellUsbdClosePipe 0x2F82F1A5 BE'),
    ]
    
    found_any = False
    for section_type, section_info in [
        ('LOAD segments', range(e_phnum)),
    ]:
        for i in range(e_phnum):
            p_off = e_phoff + i * e_phentsize
            ph = elf[p_off:p_off+e_phentsize]
            p_type = struct.unpack(endian + 'I', ph[0:4])[0]
            if p_type != 1:  # PT_LOAD only
                continue
            p_offset = struct.unpack(endian + 'Q', ph[8:16])[0]
            p_vaddr = struct.unpack(endian + 'Q', ph[16:24])[0]
            p_filesz = struct.unpack(endian + 'Q', ph[32:40])[0]
            
            seg_data = data[off + p_offset:off + p_offset + p_filesz]
            
            for nid_bytes, nid_name in nids_to_try:
                pos = 0
                while True:
                    pos = seg_data.find(nid_bytes, pos)
                    if pos < 0:
                        break
                    # Check if followed by reserved(4) + GOT_ptr(4) pattern
                    if pos + 12 <= len(seg_data):
                        triplet = seg_data[pos:pos+12]
                        reserved = struct.unpack(endian + 'I', seg_data[pos+4:pos+8])[0]
                        got_ptr = struct.unpack(endian + 'I', seg_data[pos+8:pos+12])[0]
                        phys_addr = p_vaddr + pos
                        if got_ptr > 0x00010000 and got_ptr < 0x4FFFFFFF:
                            print(f"  FOUND: {nid_name} at fileoff=0x{p_offset+pos:x} addr=0x{phys_addr:08x} reserved=0x{reserved:x} GOT=0x{got_ptr:08x}")
                            found_any = True
                        else:
                            print(f"  CANDIDATE: {nid_name} at fileoff=0x{p_offset+pos:x} addr=0x{phys_addr:08x} reserved=0x{reserved:x} GOT=0x{got_ptr:08x} (unusual GOT)")
                    pos += 1
    
    if not found_any:
        print("  No NID stubs found with known patterns")
        print("  Dumping first load segment raw for inspection...")
        for i in range(e_phnum):
            p_off = e_shoff if e_shnum > 0 else e_phoff
            # Just dump the text section or first load segment beginning
            pass

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 dump_elf.py <path_to_elf>")
        sys.exit(1)
    parse_elf(sys.argv[1])
