import struct, sys

data = open(sys.argv[1], 'rb').read()
target = int(sys.argv[2], 16)
results = []

for i in range(0, len(data) - 3, 4):
    instr = struct.unpack_from('>I', data, i)[0]
    if (instr & 0xFC000003) == 0x48000001:  # bl
        li = (instr & 0x03FFFFFC) >> 2
        if li & 0x800000:
            li = li - 0x1000000
        tgt = (i + 0x10000) + (li << 2)
        if tgt == target:
            results.append((i, i + 0x10000))

for foff, va in results:
    print(f"File 0x{foff:06X}  VA 0x{va:06X}")
print(f"\nTotal: {len(results)} callers")
