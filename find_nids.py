#!/usr/bin/env python3
"""Search for known cellUsbd NIDs in libusbd ELF."""
import sys

with open(sys.argv[1], 'rb') as f:
    data = f.read()

# Known cellUsbd NIDs
NIDS = {
    0x7F5F00D3: 'cellUsbdOpenPipe',
    0x1AB6D80B: 'cellUsbdInterruptTransfer',
    0x7B4436CE: 'cellUsbdClosePipe',
    0x2F82F1A5: 'cellUsbdGetDeviceDescriptor',
}

print(f"File size: {len(data)} bytes")

for nid, name in NIDS.items():
    pattern = bytes([(nid >> 24) & 0xFF, (nid >> 16) & 0xFF,
                     (nid >> 8) & 0xFF, nid & 0xFF])
    pos = 0
    found = False
    while True:
        pos = data.find(pattern, pos)
        if pos == -1:
            break
        found = True
        # Show full 64-byte context
        start = max(0, pos - 32)
        end = min(len(data), pos + 32)
        ctx = data[start:end]
        hexstr = ' '.join(f'{b:02x}' for b in ctx)
        print(f"\n{name} (0x{nid:08X}):")
        print(f"  File offset: 0x{pos:X}")
        if pos >= 0xF0:  # In text segment
            print(f"  Text vaddr:  0x{pos - 0xF0:X}")
        print(f"  Context [{start:X}-{end:X}]:")
        for i in range(0, len(ctx), 16):
            line = ctx[i:i+16]
            hs = ' '.join(f'{b:02x}' for b in line)
            asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in line)
            print(f"    {start+i:08X}: {hs:48s} {asc}")
        pos += 1
    if not found:
        print(f"\n{name} (0x{nid:08X}): NOT FOUND in file!")

# Also search for function address table by looking at the 
# structure right after the NID entries
print("\n=== SEARCHING FOR ADDRESS TABLE NEAR FNIDS ===")
# The FNID table entries are typically followed by address entries
# Let's look at the area around the first NID hit
for nid, name in NIDS.items():
    pattern = bytes([(nid >> 24) & 0xFF, (nid >> 16) & 0xFF,
                     (nid >> 8) & 0xFF, nid & 0xFF])
    pos = data.find(pattern)
    if pos == -1:
        continue
    # The corresponding function address might be 4 bytes at some offset
    # In SCE PRX format, the address table follows the NID table
    # Look for plausible code addresses (0x0000-0x9800) near the NID
    print(f"\n{name}: looking for address near NID at 0x{pos:X}")
    # Show the 64 bytes starting at a known offset from the NID
    for check_off in range(pos - 64, pos + 64, 4):
        if check_off < 0 or check_off + 4 > len(data):
            continue
        val = int.from_bytes(data[check_off:check_off+4], 'big')
        # Code addresses are in range 0x0000-0x9800
        if 0x100 < val < 0x9800:
            print(f"  offset 0x{check_off:X}: potential addr 0x{val:08X}")
