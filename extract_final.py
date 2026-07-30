#!/usr/bin/env python3
"""Complete auto-extraction of cellUsbd offsets using cross-referencing:
   FNID table -> OPD table -> code addresses.
   Also searches lusbd_stub.a for import FNIDs to name the exports."""
import struct, sys, os

def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def u64(d, o): return struct.unpack_from('>Q', d, o)[0]

elf_path = sys.argv[1]
with open(elf_path, 'rb') as f:
    elf = f.read()

# ============================================================
# Step 1: Build OPD -> code address map
# ============================================================
data_s = 0x98F0  # data section file offset, vaddr 0x9800
opd_map = {}  # OPD_vaddr -> code_vaddr

for off in range(data_s, data_s + 0x380, 24):
    opd_va = off - 0xF0
    code = u64(elf, off)
    code_lo = code & 0xFFFFFFFF
    code_hi = (code >> 32) & 0xFFFFFFFF
    
    cv = 0
    if 0 < code_lo < 0x9800:
        cv = code_lo
    elif 0 < code_hi < 0x9800:
        cv = code_hi
    elif 0 < code < 0x9800:
        cv = code
    
    if cv > 0:
        opd_map[opd_va] = cv

print(f"OPD map: {len(opd_map)} entries")

# ============================================================
# Step 2: Parse _sceModuleInfo -> find export table
# ============================================================
base = 0x95D4
ent_top = u32(elf, base + 36)
ent_end = u32(elf, base + 40)

print(f"Export table: vaddr 0x{ent_top:04X}-0x{ent_end:04X}")

# ============================================================
# Step 3: Parse export entries (28-byte format)
#   u16 size, u16 lib_ver, u32 attr, u16 fn_count, u16 var_count,
#   u32 unk, u32 unk2, u32 fnid_ptr, u32 addr_ptr
# ============================================================
def u16(d, o): return struct.unpack_from('<H', d, o)[0]  # LE in export table!

exports_fnid = []  # (fnid, opd_vaddr)
exports_code = []  # (fnid, code_vaddr)

off = 0xF0 + ent_top
end = 0xF0 + ent_end
entry_num = 0

while off + 28 <= end:
    size = u16(elf, off)
    if size < 16 or size > 256:
        off += 4
        continue
    
    lib_ver = u16(elf, off + 2)
    attr = u32(elf, off + 4)
    fn_count = u16(elf, off + 8)
    var_count = u16(elf, off + 10)
    
    fnid_ptr = u32(elf, off + 20)
    addr_ptr = u32(elf, off + 24)
    
    # Validate pointers
    if not (0x9000 <= fnid_ptr <= 0xB800 and 0x9000 <= addr_ptr <= 0xB800):
        off += size
        continue
    
    fnid_file = 0xF0 + fnid_ptr
    addr_file = 0xF0 + addr_ptr
    
    print(f"\nEntry {entry_num}: {fn_count} funcs, {var_count} vars, FNIDs@0x{fnid_ptr:04X}, addrs@0x{addr_ptr:04X}")
    
    for i in range(fn_count):
        if fnid_file + i*4 + 4 > len(elf) or addr_file + i*4 + 4 > len(elf):
            break
        fnid = u32(elf, fnid_file + i*4)
        opd_va = u32(elf, addr_file + i*4)
        code_va = opd_map.get(opd_va, 0)
        
        exports_fnid.append((fnid, opd_va))
        exports_code.append((fnid, code_va))
        
        marker = "*" if code_va > 0 else "?"
        print(f"  [{i:2d}] FNID=0x{fnid:08X}  OPD=0x{opd_va:04X}  code=0x{code_va:04X} {marker}")
    
    entry_num += 1
    off += size

# ============================================================
# Step 4: Try to identify functions by searching stub library
# ============================================================
print("\n=== Searching for stub FNID references ===")

# Look for lusbd_stub.a in SDK paths
stub_paths = [
    r"C:\usr\local\cell\target\ppu\lib\lusbd_stub.a",
    r"C:\usr\local\cell\host-win32\ppu\ppu-lv2\lib\lusbd_stub.a",
    r"/usr/local/cell/target/ppu/lib/lusbd_stub.a",
]

# Known cellUsbd function names we care about
TARGET_FUNCTIONS = [
    'cellUsbdOpenPipe',
    'cellUsbdInterruptTransfer', 
    'cellUsbdClosePipe',
    'cellUsbdGetDeviceDescriptor',
    'cellUsbdControlTransfer',
]

# Try to find the stub library and extract FNIDs
stub_found = None
for sp in stub_paths:
    if os.path.exists(sp):
        stub_found = sp
        break
# Also check WSL path
if not stub_found:
    wsl_path = r"/mnt/c/usr/local/cell/target/ppu/lib/lusbd_stub.a"
    if os.path.exists(wsl_path):
        stub_found = wsl_path

if stub_found:
    print(f"Found stub: {stub_found}")
    with open(stub_found, 'rb') as f:
        stub = f.read()
    
    # Search for function name strings in stub
    for func_name in TARGET_FUNCTIONS:
        pos = stub.find(func_name.encode())
        if pos >= 0:
            print(f"  Found '{func_name}' at stub offset 0x{pos:X}")
            # Look for nearby FNID (32-bit value that matches our exports)
            for check in range(pos - 32, pos + 64, 4):
                if check >= 0 and check + 4 <= len(stub):
                    val = u32(stub, check)
                    for fnid, code_va in exports_code:
                        if val == fnid:
                            print(f"    -> FNID match: 0x{fnid:08X} (code=0x{code_va:04X})")
else:
    print("Stub library not found in standard paths")

# ============================================================
# Step 5: Output best-effort offset table
# ============================================================
print("\n" + "="*60)
print("BEST-EFFORT OFFSET TABLE")
print("="*60)
print("// Generated from libusbd.sprx firmware 4.93")
print("// FNIDs cross-referenced with OPD->code mapping")
print()

# Sort by code address and list all exports
sorted_exports = sorted([(fnid, cv) for fnid, cv in exports_code if cv > 0], key=lambda x: x[1])

print(f"Total identified exports: {len(sorted_exports)}")
print()
for i, (fnid, cv) in enumerate(sorted_exports):
    print(f"// export[{i:2d}]: FNID=0x{fnid:08X} code=0x{cv:04X}")

print()
print("// The 5 cellUsbd functions we need are among the first ~11 exports above.")
print("// Without stub FNID matching, exact mapping requires Ghidra GUI or")
print("// runtime testing with known USB device behavior.")
