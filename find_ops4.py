import struct
data = open(r'c:\Users\Admin\source\repos\dimensions plugin\EBOOT.elf','rb').read()
seg1 = data[0:0x01B2EDE8]
results = []
for off in range(0, len(seg1)-16, 4):
    vals = struct.unpack('>IIII', seg1[off:off+16])
    name_ptr, p1, p2, p3 = vals
    if not (0x00010000 <= name_ptr <= 0x01D00000): continue
    if not all(0x00010000 <= v <= 0x02000000 for v in (p1,p2,p3)): continue
    name_off = name_ptr - 0x00010000
    if name_off < 0 or name_off >= len(seg1) - 4: continue
    name_end = seg1.find(b'\x00', name_off)
    if name_end < 0: continue
    name = seg1[name_off:name_end].decode('ascii','ignore')
    if len(name) < 3: continue
    va = 0x00010000 + off
    results.append((va, name, p1, p2, p3))
print(f'Found {len(results)} candidates')
for va, name, p1, p2, p3 in results:
    nl = name.lower()
    if any(kw in nl for kw in ['usb','pad','portal','toy','hid','ldd','driver','peripheral','gamepad','dimension','lego','nfc','rfid','base','pad_','_pad']):
        print(f'  VA 0x{va:08X}: name="{name}" ptrs=[0x{p1:08X},0x{p2:08X},0x{p3:08X}]')
