import struct, sys
data = open(sys.argv[1], 'rb').read()
def u32(d,o): return struct.unpack_from('>I', d, o)[0]
def u64(d,o): return struct.unpack_from('>Q', d, o)[0]

base = 0x95D4
print('=== _sceModuleInfo at 0x{:X} ==='.format(base))
labels = {0:'modattr+ver',4:'name0',8:'name1',12:'name2',16:'name3',20:'name4',24:'name5',28:'name6+term',32:'gp_value',36:'ent_top',40:'ent_end',44:'stub_top',48:'stub_end'}
for i in range(0, 56, 4):
    v = u32(data, base+i)
    l = labels.get(i, '')
    print('  +{:2d}=0x{:08X} {}'.format(i, v, l))

# 32-bit pointers: ent_top=0x9474, ent_end=0x94AC
exp_s = 0xF0 + 0x9474
exp_e = 0xF0 + 0x94AC
print('\nExport table (32-bit ptrs): {} bytes'.format(exp_e - exp_s))
exp = data[exp_s:exp_e]
for i in range(0, len(exp), 16):
    hs = ' '.join('{:02x}'.format(b) for b in exp[i:i+16])
    print('  {:08X}: {}'.format(exp_s+i, hs))

stub_s = 0xF0 + 0x94B4
stub_e = 0xF0 + 0x94E0
print('\nStub table: {} bytes'.format(stub_e - stub_s))
stub = data[stub_s:stub_e]
for i in range(0, len(stub), 16):
    hs = ' '.join('{:02x}'.format(b) for b in stub[i:i+16])
    print('  {:08X}: {}'.format(stub_s+i, hs))

print('\n64-bit pointer interpretation:')
for i in [32,40,48,56,64]:
    v = u64(data, base+i)
    ln = {32:'gp',40:'ent_top',48:'ent_end',56:'stub_top',64:'stub_end'}
    print('  +{}: 0x{:016X} {}'.format(i, v, ln.get(i,'')))
