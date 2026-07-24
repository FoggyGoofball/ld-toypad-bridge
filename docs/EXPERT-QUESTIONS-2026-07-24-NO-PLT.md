# Expert Question — 2026-07-24: No PLT Stubs or NIDs in Decrypted ELF

## What We Did

Decrypted `EBOOT.BIN` → `EBOOT.elf` using `oscetool -d` (success, 29.8MB).
Ran `analyze_eboot.py`, `dump_elf.py`, and custom Python scanners against it.

## Evidence

### 1. ELF is valid and decrypted

Entry point at 0x01B40E30. Code at file offset 0x1000 is valid PowerPC:
```
60 00 00 00 63 83 00 00 48 19 74 69 ...  → nop; ori ...; bl ...
```

### 2. Zero cellUsbd NIDs in entire 29.8MB file

```
cellUsbdInit              (0x7F5F00D3): NOT FOUND
cellUsbdOpenPipe          (0x1AB6D80B): NOT FOUND
cellUsbdInterruptTransfer (0x7B4436CE): NOT FOUND
cellUsbdClosePipe         (0x2F82F1A5): NOT FOUND
```

### 3. Zero PLT stubs in entire first LOAD segment (28.5MB)

Searched for ALL variants of `lis rX,hi / lwz rX,lo(rX) / mtctr rX / bctr`
(any register r0-r31). Also searched for `mtctr r12 + bctr` pairs. **Zero matches.**

### 4. scelibstub only has 4 imports — NOT cellUsbd

SH[14] (sceStub.rodata header, 0x28 bytes):
```
+0x00: 0x00000024  (size=36)
+0x04: 0x13BCC5F6  (library hash — not cellUsbd)
```

SH[15] (sceStub.rodata data, 0x40 bytes):
```
+0x00: 0x00000040  (size=64)
+0x04: 0x1B434CEC  (library hash — not cellUsbd)
+0x08: 0x00000004  (4 imports)
+0x10: 0x019348A4  (NID table ptr)
+0x18: 0x019348AC  (GOT table ptr)
```

Reading the NID table at 0x019348A4:
```
NID[0]: 0x00000000
NID[1]: 0x00000000
NID[2]: 0x2C000001
NID[3]: 0x0009001A
```
These are NOT cellUsbd NIDs.

### 5. No other scelibstub entries

Only two program headers of types 0x60000001/0x60000002 (scelibstub):
- PH[6]: vaddr=0x1b3ed80, fsz=0x28
- PH[7]: vaddr=0x1b3eda8, fsz=0x40

No other scelibstub sections in the ELF.

## Question

LEGO Dimensions appears to NOT dynamically import cellUsbd via the standard
scelibstub/PLT/GOT mechanism. There are no NIDs, no PLT stubs, and no GOT
slots for cellUsbd in the decrypted ELF.

1. Is cellUsbd statically linked into this game's EBOOT? If so, how do we
   hook statically-linked functions? (Inline patching at call sites?)

2. Or does the game load `libusbd.sprx` at runtime via `sys_prx_load_module`
   and resolve functions through a different API (e.g., function pointer
   tables rather than GOT)?

3. If the game uses runtime module loading, can we intercept the
   `sys_prx_load_module` call to inject our hooks when libusbd loads?

4. Alternatively, should we abandon the GOT-overwrite approach entirely
   and use a different hooking strategy:
   a) LDD emulation (register our SPRX as a USB LDD driver)?
   b) syscall hooking (intercept USB syscalls at the LV2 level)?
   c) Pattern-scan the game's .text for call sites to cellUsbd and patch them?
