#!/usr/bin/env python3
"""
analyze_eboot.py — Analyze EBOOT.BIN for NID tables and PLT stubs

PS3 SELF files have .sceStub.rodata in cleartext (signed, not encrypted).
This script searches the raw SELF for:
  1. NID values as 32-bit big-endian words
  2. PLT stub patterns (PowerPC lis/lwz/mtctr/bctr)
  3. GOT pointers near NID tables
"""

import struct
import sys
import os

# Known cellUsbd NIDs (big-endian values as they appear in memory)
NIDS = {
    0x7F5F00D3: "cellUsbdInit",
    0x1AB6D80B: "cellUsbdOpenPipe", 
    0x7B4436CE: "cellUsbdInterruptTransfer",
    0x2F82F1A5: "cellUsbdClosePipe",
}


def find_nids(data, base_offset=0):
    """Search raw data for NID values as BE 32-bit integers."""
    results = {}
    
    for nid, name in NIDS.items():
        nid_bytes = struct.pack(">I", nid)
        pos = 0
        found = []
        while True:
            pos = data.find(nid_bytes, pos)
            if pos == -1:
                break
            file_addr = base_offset + pos
            found.append(file_addr)
            pos += 4
        
        results[nid] = {"name": name, "locations": found}
    
    return results

def find_plt_stubs(data, base_offset=0):
    """Search for PowerPC PLT stub pattern."""
    results = []
    i = 0
    while i < len(data) - 16:
        # Check if this starts a PLT stub
        if data[i] == 0x3D and data[i+1] == 0x80:  # lis r12
            if data[i+4] == 0x81 and data[i+5] == 0x8C:  # lwz r12, lo(r12)
                if data[i+8:i+12] == b'\x7D\x89\x03\xA6':  # mtctr r12
                    if data[i+12:i+16] == b'\x4E\x80\x04\x20':  # bctr
                        hi = (data[i+2] << 8) | data[i+3]
                        lo = (data[i+6] << 8) | data[i+7]
                        got_addr = (hi << 16) | lo
                        file_addr = base_offset + i
                        results.append({
                            "file_addr": file_addr,
                            "instructions": f"lis r12, 0x{hi:04X} / lwz r12, 0x{lo:04X}(r12) / mtctr / bctr",
                            "got_addr": got_addr,
                        })
                        i += 16
                        continue
        i += 1
    
    return results

def dump_context(data, file_addr, size=64):
    """Dump hex context around a file offset."""
    start = max(0, file_addr - size)
    end = min(len(data), file_addr + size)
    
    result = []
    for i in range(start, end, 4):
        if i + 4 <= end:
            word = struct.unpack(">I", data[i:i+4])[0]
            marker = ""
            if word in NIDS:
                marker = f" <-- {NIDS[word]}"
            elif word >= 0x00100000 and word <= 0x04FFFFFF:
                marker = " <-- possible addr ptr"
            result.append((i, word, marker))
    
    return result

def main():
    if len(sys.argv) < 2:
        print("Usage: python analyze_eboot.py <EBOOT.BIN>")
        sys.exit(1)
    
    path = sys.argv[1]
    if not os.path.exists(path):
        print(f"ERROR: {path} not found")
        sys.exit(1)
    
    with open(path, "rb") as f:
        data = f.read()
    
    print(f"=== Analyzying {path} ({len(data)} bytes, {len(data)/1024/1024:.1f}MB) ===")
    print()
    
    # Check for ELF magic
    elf_pos = data.find(b'\x7fELF')
    if elf_pos >= 0:
        ei_class = data[elf_pos + 4]
        ei_data = data[elf_pos + 5]
        e_type = struct.unpack(">H", data[elf_pos+16:elf_pos+18])[0]
        e_machine = struct.unpack(">H", data[elf_pos+18:elf_pos+20])[0]
        
        endian = "BE" if ei_data == 2 else "LE"
        etype = {2: "EXEC", 3: "DYN"}.get(e_type, f"unknown({e_type})")
        mach = {20: "PPC"}.get(e_machine, f"unknown({e_machine})")
        
        print(f"ELF header at offset 0x{elf_pos:X}:")
        print(f"  Class:    {'ELF32' if ei_class == 1 else 'ELF64' if ei_class == 2 else 'unknown'}")
        print(f"  Endian:   {endian}")
        print(f"  Type:     {etype}")
        print(f"  Machine:  {mach}")
        print()
    
    # Check for SCE header
    sce_pos = data.find(b'\x534345')  # "SCE"
    if sce_pos >= 0:
        print(f"SCE/SELF header at offset 0x{sce_pos:X}")
        print()
    
    print("=== Step 1: Search for NID values ===")
    nid_results = find_nids(data)
    all_found = True
    
    for nid, info in nid_results.items():
        locs = info["locations"]
        if locs:
            print(f"\n  ✓ {info['name']} (0x{nid:08X}): {len(locs)} occurrence(s):")
            for loc in locs:
                print(f"      0x{loc:08X}")
                # Show context
                ctx = dump_context(data, loc, 32)
                for addr, word, marker in ctx:
                    mark = marker if marker else ""
                    print(f"        0x{addr:08X}: 0x{word:08X}{mark}")
        else:
            print(f"\n  ✗ {info['name']} (0x{nid:08X}): NOT FOUND")
            all_found = False
    
    print()
    print("=== Step 2: Search for PLT stubs ===")
    plt_stubs = find_plt_stubs(data)
    print(f"  Found {len(plt_stubs)} PLT stubs")
    
    # Show first 10
    for stub in plt_stubs[:10]:
        print(f"  0x{stub['file_addr']:08X}: {stub['instructions']}  (GOT=0x{stub['got_addr']:08X})")
    
    if len(plt_stubs) > 10:
        print(f"  ... and {len(plt_stubs) - 10} more")
    
    print()
    
    if all_found:
        print("=== Step 3: Analyze NID table structure ===")
        # Get first occurrence of each NID
        first_locs = {}
        for nid, info in nid_results.items():
            if info["locations"]:
                first_locs[nid] = info["locations"][0]
        
        # Sort by location
        sorted_locs = sorted(first_locs.items(), key=lambda x: x[1])
        
        print("  NID table layout (first occurrence of each):")
        for nid, loc in sorted_locs:
            name = NIDS[nid]
            # Read 3 words starting at this NID
            # Expected: {NID, reserved(0), GOT_ptr}
            if loc + 12 <= len(data):
                nid_val = struct.unpack(">I", data[loc:loc+4])[0]
                reserved = struct.unpack(">I", data[loc+4:loc+8])[0]
                got_ptr = struct.unpack(">I", data[loc+8:loc+12])[0]
                print(f"    0x{loc:08X}: NID=0x{nid_val:08X} reserved=0x{reserved:08X} GOT_ptr=0x{got_ptr:08X} ({name})")
                
                # If GOT_ptr looks valid, read it
                if 0x00100000 <= got_ptr <= 0x04FFFFFF:
                    # Calculate file offset from vaddr
                    # For PS3 ELF, vaddr 0x00010000 -> file offset 0
                    file_ofs = got_ptr - 0x00010000  # rough
                    if 0 <= file_ofs < len(data):
                        val = struct.unpack(">I", data[file_ofs:file_ofs+4])[0]
                        resolved = "RESOLVED" if val >= 0x30000000 else "UNRESOLVED/PLT"
                        print(f"                        *GOT_ptr = 0x{val:08X} ({resolved})")
        
        # Check if NIDs are part of a parallel array structure
        print()
        print("  Checking for scelibstub structure...")
        
        # The scelibstub format uses parallel arrays:
        #   uint32_t nids[]  - NID table
        #   uint32_t addrs[] - GOT pointer table
        # stored as separate arrays, not interleaved
        
        min_loc = min(loc for _, loc in sorted_locs)
        max_loc = max(loc for _, loc in sorted_locs)
        span = max_loc - min_loc
        
        print(f"  NID locations span: 0x{min_loc:08X} - 0x{max_loc:08X} ({span} bytes)")
        
        if span <= 16:
            print("  → NIDs are tightly packed (original triplet format assumed)")
        else:
            print("  → NIDs are spread apart (parallel array format assumed)")
        
        # Look for sceStub header
        for nid, loc in sorted_locs:
            for offset in range(-64, 0, 4):
                if loc + offset >= 0:
                    word = struct.unpack(">I", data[loc+offset:loc+offset+4])[0]
                    if word == 0x00000001 or word == 0x00020001:
                        print(f"  Possible scelibstub header at 0x{loc+offset:08X}: 0x{word:08X}")
                        break
    
    print()
    print("=== Summary ===")
    
    # Determine the base virtual address of the first LOAD segment
    # Standard PS3 ELF: first segment vaddr = 0x00010000
    elf_base = 0x00010000
    
    if nid_results[0x7F5F00D3]["locations"]:
        # Convert file offsets to virtual addresses
        print("  NID tables found! Virtual addresses (for scan regions):")
        for nid, info in nid_results.items():
            if info["locations"]:
                first_loc = info["locations"][0]
                vaddr = elf_base + first_loc
                print(f"    {info['name']}: file=0x{first_loc:08X} -> vaddr=0x{vaddr:08X}")
                
                # Also look for the GOT pointer at +8
                loc = first_loc
                if loc + 12 <= len(data):
                    _, _, got_ptr = struct.unpack(">III", data[loc:loc+12])
                    if got_ptr:
                        file_ofs = got_ptr - elf_base
                        val = struct.unpack(">I", data[file_ofs:file_ofs+4])[0] if 0 <= file_ofs < len(data) else 0
                        resolved_str = " (resolved in file)" if val >= 0x30000000 else " (unresolved PLT in file)"
                        print(f"      GOT ptr: 0x{got_ptr:08X} -> file_ofs 0x{file_ofs:08X} -> *val = 0x{val:08X}{resolved_str}")
    
    if plt_stubs:
        first_plt = plt_stubs[0]
        print(f"\n  First PLT stub at file offset 0x{first_plt['file_addr']:08X}")
        vaddr = elf_base + first_plt['file_addr']
        print(f"    Virtual address: 0x{vaddr:08X}")
        print(f"    GOT address:     0x{first_plt['got_addr']:08X}")
    
    print("\nDone.")

if __name__ == "__main__":
    main()
