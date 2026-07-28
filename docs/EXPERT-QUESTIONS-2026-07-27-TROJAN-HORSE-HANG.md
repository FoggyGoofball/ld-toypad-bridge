# Expert Questions — 2026-07-27: Trojan Horse Partial Success → Visual Corruption & Hang

## Current State

The game boots, reaches "Connect Toy Pad" screen. After PS3MAPI injection at T+60s, our SPRX installs 4 NID-based GOT hooks (OpenPipe, InterruptTransfer, ClosePipe, GetDeviceDescriptor). When a USB drive is physically plugged into the PS3, **the game reacts** (visual corruption + hang, but can exit to XMB). 

This proves the Kernel → probe → GetDeviceDescriptor chain fired successfully. The attach/OpenPipe/Transfer phase is failing.

### Architecture (current code)

| # | Hook | Trampoline Offset | Purpose |
|---|------|-------------------|---------|
| 0 | `cellUsbdOpenPipe` | 0 | Fake pipe handles for ToyPad endpoints (0x81/0x01) |
| 1 | `cellUsbdInterruptTransfer` | 64 | HID MITM (calls `network_send_poll` + `network_recv`) |
| 2 | `cellUsbdClosePipe` | 128 | Clean up fake pipes |
| 3 | `cellUsbdGetDeviceDescriptor` | 192 | Trojan Horse: lie about VID/PID (returns 0x0E6F:0x0241) |

- Heartbeat at offset 256
- All GOT overwrites via NID scan (no cellUsbd function calls, no ping-and-scan)
- `cellUsbdInit` hook removed (expert recommendation 2026-07-26)

---

## Questions

### 1. Logging Strategy

Our hooks currently use `DEBUG_PRINT()` which writes to a remote UDP debug port. Should we also add `papertrail()` calls (writing to `/dev_hdd0/plugins/ldtoypad_boot.log`) at each hook entry point so we can see the exact call sequence even when the PC server isn't running? 

What's the recommended pattern — log on entry with args, log on exit with result? Should we log both in the same call or use a paired entry/exit pattern?

```c
// Proposed:
int my_cellUsbdOpenPipe(...) {
    papertrail("[USB] ENTER OpenPipe dev=0x%08X ep=0x%02X", dev_id, ep_addr);
    // ... hook logic ...
    papertrail("[USB] EXIT OpenPipe pipe=0x%08X ret=%d", pipe_handle, ret);
    return ret;
}
```

### 2. InterruptTransfer Network Dependency

Our `my_cellUsbdInterruptTransfer` hook calls `network_send_poll()` followed by `network_recv()` for IN transfers:

```c
if (toypad_type == 1) {  // IN endpoint
    network_send_poll(zone, seq);     // Tell server we want data
    recv_len = network_recv(response, sizeof(response));  // Block until response
    // ... copy response to game buffer ...
}
```

If the PC server (UDP port 28472) isn't running or responding, the game's USB thread **blocks indefinitely** on `network_recv()`. 

**Question:** Could this blocking cause the visual corruption we're seeing (game rendering stalls while waiting for USB thread)? On PS3, does the game's rendering depend on the USB thread completing its poll cycle?

**Should we add a timeout or return empty data immediately if the server isn't connected?** For example:
- Add a `g_server_connected` flag set by the main loop when it receives a keepalive from the server
- If not connected, return empty data immediately (memset zero) instead of blocking

### 3. Control Transfer Handling

When the game attaches to the fake ToyPad, does it also perform **USB control transfers** (Get Descriptor, Set Configuration, Get HID Report Descriptor, etc.) BEFORE opening interrupt pipes?

We only hook `cellUsbdInterruptTransfer`. If the game calls `cellUsbdControlTransfer` first (to read the configuration descriptor, interface descriptor, endpoint descriptors, HID report descriptor), those calls pass through to the **real OS** — which would return the real flash drive's descriptors, potentially confusing the game.

**Should we also hook `cellUsbdControlTransfer`?** The game's NID table presumably has entries for it. We'd need to intercept descriptor requests and return ToyPad-specific data (configuration, HID report, string descriptors).

### 4. GOT Resolution Timing

At T+60s (PS3MAPI injection), the game has been at the "Connect ToyPad" screen for ~55 seconds. Has it already called `cellUsbdOpenPipe`, `cellUsbdInterruptTransfer`, or `cellUsbdClosePipe`?

If the GOT slots are resolved to libusbd.sprx addresses at injection time, our NID scan will find them but **skip the overwrite** (the `plt_addr >= 0x30000000` check in `get_game_got_slot`).

**How can we verify whether the GOT slots are resolved at injection time?** Can we use PS3MAPI `/getmem` to read the GOT slot values? Or should we add logging to report the resolution state of each slot?

### 5. Visual Corruption Cause

The user reports "visual corruption" (not a black screen or hard crash) when the USB drive is plugged. The game's USB thread is blocked waiting for our network response, but the main rendering thread is still running.

**Possible causes:**
- A) Blocked USB thread causing rendering pipeline stalls (PS3 game engines may synchronize on USB completion)
- B) Our OpenPipe hook returning a bad pipe handle that the game later uses to write garbage
- C) The game calling ControlTransfer (unhooked) and getting real flash drive descriptors, then trying to configure endpoints that don't match our fake pipe handles

**Which is most likely?** How would we distinguish between these?

### 6. Minimal Viable Test

For a minimal test to isolate the issue, should we modify `my_cellUsbdInterruptTransfer` to **return empty data immediately** (no network calls) so the game doesn't block on the network?

```c
// Proposed minimal test:
if (toypad_type == 1) {  // IN endpoint
    memset(buf, 0, max_len);
    ((uint8_t*)buf)[0] = 0x01;  // Dummy status byte
    *len = 1;
    return CELL_OK;
}
```

This would isolate whether the visual corruption is from:
- Network blocking (vs. pipe handle corruption)
- Missing ControlTransfer hooks

### 7. PS3MAPI Pre-Injection Verification

Before injecting, can we use PS3MAPI `/getmem` to:
- Read the game's GOT slots for the 4 NIDs to check if they're resolved
- Read the game's memory at the NID scan range to verify the NID table exists
- Verify the game PID is correct (0x1010200 for BLUS31473)

This would confirm whether injection at T+60s is too late (GOT already resolved).

---

## Environment

| Item | Value |
|------|-------|
| PS3 | CECH-2501A, Cobra CFW 4.91, webMAN MOD 1.47.48q |
| PS3 IP | 192.168.0.22 |
| Game | LEGO Dimensions BLUS31473, PID 0x1010200 |
| SDK | Sony DUPLEX SDK 3.40, ppu-lv2-gcc |
| SPRX | /dev_hdd0/plugins/ldtoypad.sprx (23,088 bytes) |
| Injection | PS3MAPI at T+60s |
| PC Server | ld-toypad-server, UDP port 28472 |
| boot_plugins.txt | Only webftp_server.sprx (clean) |
| EBOOT | Verified clean (SHA256: 2378019D...) |
