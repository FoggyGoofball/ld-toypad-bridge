# Expert Question — 2026-07-25: LDD Registered — Next Steps for ToyPad Interception

## Status: LDD Registration SUCCESSFUL

After resolving the diagnostic issue (stale papertrail files masking real
boot log output), we confirmed the full init chain works:

```
=== ldtoypad module_start ===
OK: worker thread created, returning resident
=== worker_thread started ===
OK: debug_init()
OK: network_init(28472)
OK: network_wait_ready()
OK: network_set_server(192.168.0.17:28472)
OK: ldd_driver_init() — ToyPad LDD REGISTERED
OK: toypad_state_init()
=== Entering main loop ===
```

Debug log confirms:
```
[LDD] cellSysmoduleLoadModule(USBD) OK
[LDD] cellUsbdInit: USB already initialized, continuing
[LDD] Extra LDD registered OK
```

Key fixes applied:
- `cellSysmoduleLoadModule(CELL_SYSMODULE_USBD)` before `cellUsbdInit()`
- `cellUsbdInit` returning `0x80110002` (already initialized) treated as success
- Return value check relaxed to `ret < 0` instead of exact match

## Current Architecture

```c
static CellUsbdLddOps g_ldd_ops = {
    .name   = "ldtoypad",
    .probe  = ldd_probe,    // return 0 to claim device
    .attach = ldd_attach,   // set up pipes
    .detach = ldd_detach,   // clean up
};

// In attach():
g_ldd.device.ep_addr_in  = 0x81;   // USB interrupt IN endpoint
g_ldd.device.ep_addr_out = 0x01;   // USB interrupt OUT endpoint
g_ldd.device.pipe_in  = dev_id | 0x100;
g_ldd.device.pipe_out = dev_id | 0x200;
```

## Questions

### 1. Will probe/attach fire automatically?

Once `cellUsbdRegisterExtraLdd` succeeds, will the kernel automatically
call our `ldd_probe` and `ldd_attach` when the ToyPad is connected? Or
do we need to call `cellUsbdScanDevice()` or similar to trigger detection?

The ToyPad is connected via USB at all times (it's a wired portal).
We expect probe/attach to fire during game startup when USB is initialized.

### 2. How do we receive USB interrupt transfer data?

After `ldd_attach` fires and we have pipe handles, how do we read data
from the ToyPad's interrupt IN endpoint (0x81)?

Options we've seen:
a) `cellUsbdInterruptReceive(pipe, buf, size, callback, arg)` — async
b) `cellUsbd BulkTransfer` — but ToyPad uses interrupt, not bulk
c) Direct LV2 syscall `sys_usbd_receive` — bypass CellOS abstraction

Which API is correct for the Sony SDK?

### 3. How do we send data to the ToyPad?

To emulate tags/vehicles, we need to send data to the ToyPad's interrupt
OUT endpoint (0x01). What's the correct API?

a) `cellUsbdInterruptSend(pipe, buf, size, callback, arg)`
b) Something else?

### 4. UDP Bridge Architecture

Our plan: receive ToyPad data via interrupt IN pipe → forward to PC via
UDP sendto (port 28472). Receive PC commands via UDP recvfrom → forward
to ToyPad via interrupt OUT pipe.

Is there anything fundamentally wrong with this approach? Do the USB
callbacks run in a context where UDP sendto is safe?

### 5. Alternative if Extra LDD doesn't intercept data

If `cellUsbdRegisterExtraLdd` only registers a "driver" but doesn't
actually redirect data to our callbacks (the game's own USB handlers
might still receive data), what's the fallback?

- Hook `cellUsbdInterruptReceive` in the game's import table?
- Use a kernel-level USB hook via Cobra payload?

## Environment
- PS3 CECH-2501A, Cobra CFW 4.91, webMAN MOD 1.47.48q
- Injection: PS3MAPI MODULE LOAD into game PID 0x1010200
- SPRX: Sony DUPLEX SDK 3.40, ppu-lv2-gcc -mprx -O2
- Links: -llv2_stub -lfs_stub -lnet_stub -lusbd_stub -lsysmodule_stub
