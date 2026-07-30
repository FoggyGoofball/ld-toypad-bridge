#!/usr/bin/env python3
"""Automated extraction of cellUsbd function offsets from libusbd ELF.
Uses OPD tracing and export table parsing - no external dependencies."""
import struct, sys

def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def u64(d, o): return struct.unpack_from('>Q', d, o)[0]

with open(sys.argv[1], 'rb') as f:
    data = f.read()

# ============================================================
# Step 1: Read _sceModuleInfo at file offset 0x95D4
# ============================================================
# 32-bit pointer layout: header(32) + gp(4) + ent_top(4) + ent_end(4) + stub_top(4) + stub_end(4)
base = 0x95D4
ent_top    = u32(data, base + 36)  # vaddr of export table
ent_end    = u32(data, base + 40)
stub_top   = u32(data, base + 44)
stub_end   = u32(data, base + 48)

print(f"Export table: vaddr 0x{ent_top:04X}-0x{ent_end:04X}")
print(f"Stub table:   vaddr 0x{stub_top:04X}-0x{stub_end:04X}")

# ============================================================
# Step 2: Parse export table entries
# Each entry is 28 bytes: size(2) ver(2) attr(4) fn(2) var(2) unk(4) unk2(4) fnid_ptr(4) addr_ptr(4)
# ============================================================
def parse_export_entry(data, file_off):
    """Parse a 28-byte export library entry. Returns dict or None."""
    sz = u32(data, file_off)
    size = sz >> 16
    if size < 16 or size > 256:
        return None
    
    lib_ver = sz & 0xFFFF
    attr = u32(data, file_off + 4)
    fn_var = u32(data, file_off + 8)
    fn_count = (fn_var >> 16) & 0xFFFF
    var_count = fn_var & 0xFFFF
    
    unk2 = u32(data, file_off + 16)
    fnid_ptr = u32(data, file_off + 20)
    addr_ptr = u32(data, file_off + 24)
    
    # Validate: fnid_ptr and addr_ptr should be valid vaddrs in .rodata range
    if not (0x9000 <= fnid_ptr <= 0xB8C0 and 0x9000 <= addr_ptr <= 0xB8C0):
        return None
    
    return {
        'size': size, 'lib_ver': lib_ver, 'attr': attr,
        'fn_count': fn_count, 'var_count': var_count,
        'fnid_file': 0xF0 + fnid_ptr,
        'addr_file': 0xF0 + addr_ptr,
        'unk2': unk2,
    }

entries = []
off = 0xF0 + ent_top
end = 0xF0 + ent_end
while off + 28 <= end:
    entry = parse_export_entry(data, off)
    if entry:
        entries.append(entry)
        off += entry['size']
    else:
        off += 4  # skip and try again

print(f"\nParsed {len(entries)} export entries")

# ============================================================
# Step 3: For each entry, read FNIDs and resolve OPD -> code address
# ============================================================
# OPD format (24 bytes): code_addr(8), toc_addr(8), env_ptr(8)
def resolve_opd_to_code(data, opd_vaddr):
    """Given a vaddr in the data section pointing to an OPD, return the code vaddr."""
    file_off = 0x98F0 + (opd_vaddr - 0x9800)
    if file_off + 24 > len(data):
        return 0
    code = u64(data, file_off)
    # The code_addr in the OPD should be a valid .text vaddr (0x0000-0x97FF)
    if 0 <= code <= 0x9800:
        return code
    # Sometimes OPD code_addr is a 32-bit value in the upper/lower half
    code32 = u32(data, file_off)
    if 0 < code32 < 0x9800:
        return code32
    return 0

all_functions = []
for i, entry in enumerate(entries):
    fn_count = entry['fn_count']
    if fn_count == 0 or fn_count > 200:
        continue
    
    print(f"\nEntry {i}: {fn_count} functions, FNIDs at file 0x{entry['fnid_file']:X}, addrs at 0x{entry['addr_file']:X}")
    
    for j in range(fn_count):
        fnid_off = entry['fnid_file'] + j * 4
        addr_off = entry['addr_file'] + j * 4
        
        if fnid_off + 4 > len(data) or addr_off + 4 > len(data):
            break
        
        fnid = u32(data, fnid_off)
        opd_vaddr = u32(data, addr_off)
        code_vaddr = resolve_opd_to_code(data, opd_vaddr)
        
        if code_vaddr > 0:
            # Read first instruction at code address
            file_code = 0xF0 + code_vaddr
            first_insn = u32(data, file_code) if file_code + 4 <= len(data) else 0
            print(f"  [{j:2d}] FNID=0x{fnid:08X}  OPD@0x{opd_vaddr:04X}  code=0x{code_vaddr:04X}  insn=0x{first_insn:08X}")
            all_functions.append({
                'fnid': fnid,
                'opd_vaddr': opd_vaddr,
                'code_vaddr': code_vaddr,
                'first_insn': first_insn,
                'entry': i,
            })

# ============================================================
# Step 4: Search for function name strings to identify cellUsbd exports
# ============================================================
print("\n=== Searching for cellUsbd function name strings ===")
# The export function names may be embedded as strings nearby
# Look for ASCII strings in the data section
name_candidates = []
for off in range(0x98F0, min(len(data), 0x98F0 + 0x380)):
    # Try to read a null-terminated ASCII string
    end_s = off
    while end_s < len(data) and 32 <= data[end_s] < 127:
        end_s += 1
    if end_s - off >= 5:  # At least 5 chars
        s = data[off:end_s].decode('ascii', errors='ignore')
        if any(kw in s for kw in ['cellUsbd', 'OpenPipe', 'ClosePipe', 'Transfer', 'GetDevice', 'Control', 'RegisterLdd', 'Init']):
            vaddr = off - 0xF0
            name_candidates.append((vaddr, s))
            print(f"  vaddr 0x{vaddr:04X}: '{s}'")

# ============================================================
# Step 5: Cross-reference FNIDs with our known values from the SDK
# ============================================================
# These are the import NIDs from lusbd_stub.a - they may differ from export FNIDs
IMPORT_NIDS = {
    0x7F5F00D3: 'cellUsbdOpenPipe',
    0x1AB6D80B: 'cellUsbdInterruptTransfer',
    0x7B4436CE: 'cellUsbdClosePipe',
    0x2F82F1A5: 'cellUsbdGetDeviceDescriptor',
}

print("\n=== Known NID matches ===")
for fn in all_functions:
    if fn['fnid'] in IMPORT_NIDS:
        name = IMPORT_NIDS[fn['fnid']]
        print(f"  {name}: code=0x{fn['code_vaddr']:04X} (FNID=0x{fn['fnid']:08X})")

# ============================================================
# Step 6: List all code addresses sorted, for manual identification
# ============================================================
print("\n=== All exported function code addresses (sorted) ===")
sorted_funcs = sorted(all_functions, key=lambda x: x['code_vaddr'])
for fn in sorted_funcs:
    name_hint = IMPORT_NIDS.get(fn['fnid'], '')
    print(f"  code=0x{fn['code_vaddr']:04X}  FNID=0x{fn['fnid']:08X}  insn=0x{fn['first_insn']:08X}  {name_hint}")

print(f"\nTotal exported functions: {len(all_functions)}")
