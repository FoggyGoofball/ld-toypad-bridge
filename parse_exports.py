#!/usr/bin/env python3
"""Parse libusbd.sprx export table by tracing strings and structure."""
import sys, struct

def u32(data, off):
    return struct.unpack_from('>I', data, off)[0]

def u64(data, off):
    return struct.unpack_from('>Q', data, off)[0]

with open(sys.argv[1], 'rb') as f:
    data = f.read()

# Find key strings
for s in ['cellUsbd_Library', '4cellUsbd']:
    pos = data.find(s.encode())
    if pos >= 0:
        vaddr = pos - 0xF0
        print(f"'{s}' at file 0x{pos:X} (vaddr 0x{vaddr:X})")

# LOPROC segment: file offset 0x9C70, size 0x1C50
LOPROC = 0x9C70

print(f"\n=== Module export header at 0x{LOPROC:X} ===")
off = LOPROC
print(f"u64[0] = 0x{u64(data, off):016X}")
print(f"u64[1] = 0x{u64(data, off+8):016X}")

# The first 32-bit value 0x000092D6 — let's check what's there
v = u32(data, LOPROC)
print(f"\nFirst u32 = 0x{v:08X} — interpreting as offset within LOPROC:")
target = LOPROC + v
if target < len(data):
    ctx = data[target:target+32]
    print(f"  At LOPROC+0x{v:X} = file 0x{target:X}: {' '.join(f'{b:02x}' for b in ctx)}")

# The values 0x92D6, 0x92DA, 0x92F6 etc are close together (4 bytes apart)
# These might be OPD offsets. Each OPD on PPC64 is 24 bytes (3 x uint64).
# But 0x92DA - 0x92D6 = 4, which is too small for OPDs.
# These might instead be offsets to function ENTRY POINTS.

# Let's check if these are code addresses:
for addr in [0x92D6, 0x92DA, 0x92F6, 0x92FA, 0x9316, 0x931A]:
    file_off = 0xF0 + addr  # vaddr to file offset
    if file_off + 4 <= len(data):
        insn = u32(data, file_off)
        print(f"  vaddr 0x{addr:04X} (file 0x{file_off:X}): PPC insn = 0x{insn:08X}")

print(f"\n=== Scanning for (FNID, code_addr) pairs ===")
# In SCE PRX exports, FNIDs are stored separately from addresses.
# The typical format has a table of FNIDs followed by a table of addresses.
# Let's scan the LOPROC segment for 32-bit values that look like code addresses
addrs = []
for off in range(LOPROC, LOPROC + 0x1C50, 4):
    v = u32(data, off)
    # Code addresses are in range 0x100-0x9800
    if 0x100 <= v <= 0x9800:
        addrs.append((off, v))

print(f"Found {len(addrs)} potential code addresses in LOPROC")
for off, v in addrs[:40]:
    print(f"  0x{off:06X}: 0x{v:08X}")

print(f"\n=== Looking for FNID table (32-bit hashes > 0x10000000) ===")
fnids = []
for off in range(LOPROC, LOPROC + 0x1C50, 4):
    v = u32(data, off)
    if v > 0x10000000:
        fnids.append((off, v))

print(f"Found {len(fnids)} potential FNIDs:")
for off, v in fnids[:30]:
    print(f"  0x{off:06X}: 0x{v:08X}")
