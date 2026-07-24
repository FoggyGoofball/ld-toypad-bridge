# Expert Question — 2026-07-24 14:45: Ping-and-Scan Stub Format Mismatch

## Current Status

We implemented the "Ping and Scan" approach as recommended. The SPRX loads
and runs via PS3MAPI injection. However, `get_real_os_address()` fails for
all 4 cellUsbd functions because our import stubs don't use the expected
`lis rX, hi / lwz rX, lo(rX)` pattern.

## Evidence

### Debug Log (actual stub bytes read by get_real_os_address)

```
[USB] ping: stub=0x2c30048 [0]=0x2C20010 [1]=0x2C38340
[USB] ping: not lis
[USB] ping: stub=0x2c300a0 [0]=0x2C2044C [1]=0x2C38340
[USB] ping: not lis
[USB] ping: stub=0x2c300d0 [0]=0x2C20734 [1]=0x2C38340
[USB] ping: not lis
[USB] ping: stub=0x2c30098 [0]=0x2C203EC [1]=0x2C38340
[USB] ping: not lis
[USB] ping result: init=0x0 open=0x0 xfer=0x0 close=0x0
```

### OPD Validation (from find_cellusbd_functions_via_opd)

```
[USB] OPD: cellUsbdInit => { code=0x2C20010 toc=0x2C38340 env=... } (opd at 0x2C30048)
```

### Analysis

The function pointer `(void*)cellUsbdInit` = 0x2C30048 points to what appears
to be an OPD (Official Procedure Descriptor), not directly to stub code:

- word[0] = 0x2C20010 — this is the code_addr (same as OPD code field)
- word[1] = 0x2C38340 — this is the toc_addr (same for all 4 functions)

So `*(uint32_t*)cellUsbdInit` returns 0x2C20010, which is a code address
(not a `lis` instruction), hence the "not lis" log.

We tried following the OPD: read stub code at 0x2C20010. But the bytes at
that address (read via PS3MAPI MEMORY GET) are `08 01 0F 01 0C 08 02 06...`
which don't look like PowerPC instructions at all — they look like packed
data bytes, not the expected `lis/lwz/mtctr/bctr` pattern.

## SDK Details

- **SDK**: Sony DUPLEX SDK (SN Systems / SDK 3.40)
- **Compiler**: `ppu-lv2-gcc.exe -mprx -std=gnu99 -O2`
- **Link**: `-mprx -llv2_stub -lfs_stub -lnet_stub -lusbd_stub`
- **Signing**: oscetool 0.9.2, SELF type APP

## Question

1. On this SDK version, what is the exact format of PRX import stubs for
   `-lusbd_stub` functions? The stubs don't match the `lis/lwz` GOT-loading
   pattern. What pattern should we search for?

2. Is `(void*)cellUsbdInit` actually an OPD pointer? If so, how do we follow
   the OPD to find the resolved GOT entry? Do we need to read the stub at
   code_addr, or is there a different mechanism?

3. The debug log shows the OPD TOC (0x2C38340) is identical for all 4
   cellUsbd functions, suggesting they share a TOC. Does this provide a
   clue about the stub format (e.g., TOC-relative addressing via `addis`)?

4. If the stub format cannot be parsed from our SPRX, is there an alternative
   approach? For example:
   a) Use PS3MAPI MEMORY GET from the PC side to read the game's PLT stubs
      and extract GOT addresses offline?
   b) Use `sys_prx_get_module_info` or similar to enumerate libusbd.sprx's
      export table and get the real function addresses?
