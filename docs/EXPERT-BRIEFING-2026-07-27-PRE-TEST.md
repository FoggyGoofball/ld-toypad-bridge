# Expert Briefing — 2026-07-27: Pre-Test Status Report

## Executive Summary

All six expert recommendations from the previous round have been implemented. The game boots, reaches "Connect ToyPad," and the SPRX injects successfully via PS3MAPI (T+60s). The boot log confirms the latest build runs (`BUILD 2026-07-27-1100`). However, the game remains stuck at the "Connect ToyPad" screen after plugging in a USB device.

The most recent test showed **0/5 hooks installed** — the NID scanner was using an incorrect 3-word triplet stride. This has been fixed to 1-word stride with forward GOT pointer search. The next test will reveal whether hooks are now actually installing.

---

## Architecture (Current Code)

| Offset | Hook | TOC Reg | Purpose |
|--------|------|---------|---------|
| 0 | `cellUsbdOpenPipe` | r6 | Fake pipe handles for ToyPad endpoints (0x81/0x01) |
| 64 | `cellUsbdInterruptTransfer` | r8 | HID MITM — non-blocking UDP poll + `done_cb` callback |
| 128 | `cellUsbdClosePipe` | r4 | Clean up fake pipes |
| 192 | `cellUsbdGetDeviceDescriptor` | r5 | Trojan Horse: returns ToyPad VID/PID (0x0E6F:0x0241) |
| 256 | `cellUsbdControlTransfer` | r9 | Fake HID descriptors (configuration, interface, HID report) |
| 320 | Heartbeat counter | — | PS3MAPI health polling |

- Trampoline page: 64KB R-W-X allocation via `sys_memory_allocate`
- GOT overwrite: unconditional (no `plt_addr >= 0x30000000` check — expert recommendation)
- No `cellUsbdInit` hook (removed — calling Init from SPRX context after game USB init can destabilize the stack)
- No ping-and-scan (removed — memory corruption risk from heuristic address scanning)

---

## Fixes Applied Since Last Expert Review

### 1. `done_cb` Callback (Critical)
**Problem:** `cellUsbdInterruptTransfer` is asynchronous. The game expects `done_cb` to be called to wake its USB thread. We returned `CELL_OK` but never called `done_cb`, marooning the game's USB thread in an infinite sleep.

**Fix:** Both IN and OUT handlers now call `done_cb(CELL_OK, len, arg)` before returning:

```c
typedef void (*usbd_done_cb_t)(int32_t result, int32_t count, void *arg);

if (toypad_type == 1) {  // IN endpoint
    // ... fill buffer with data or 0x55 magic byte ...
    if (done_cb) {
        usbd_done_cb_t cb = (usbd_done_cb_t)done_cb;
        cb(CELL_OK, (int32_t)*len, arg);
    }
    return CELL_OK;
}
```

### 2. ControlTransfer Hook Added
**Problem:** Game calls `cellUsbdControlTransfer` to read configuration/HID descriptors. Without hooking, requests pass through to the real OS which returns the flash drive's Mass Storage descriptors → heap corruption → visual artifacts.

**Fix:** Added 5th hook (NID `0x3219460D`). Routes to `toypad_state_control_transfer()` which returns correct ToyPad HID descriptors:

```c
int my_cellUsbdControlTransfer(uint32_t dev_handle, void *setup_pkt,
                                void *buf, uint32_t *length,
                                void *done_cb, void *user_data,
                                uint32_t game_toc)
{
    uint8_t *sp = (uint8_t*)setup_pkt;
    uint32_t bmRequestType = sp[0], bRequest = sp[1],
             wValue = sp[2] | ((uint32_t)sp[3] << 8),
             wIndex = sp[4] | ((uint32_t)sp[5] << 8),
             wLength = sp[6] | ((uint32_t)sp[7] << 8);
    int ret = toypad_state_control_transfer(bmRequestType, bRequest,
                                             wValue, wIndex, buf, wLength);
    if (ret == 0 && wLength > 0) *length = wLength;
    return ret;
}
```

### 3. InterruptTransfer Non-Blocking + `0x55` Magic Byte
**Problem:** `network_recv()` blocked the game's USB thread.

**Fix:** Socket is `SO_NBIO` (set at `network_init`). `network_recv()` returns 0 immediately when no data. IN endpoint returns `0x55` magic byte to test LEGO Dimensions protocol handshake:

```c
if (toypad_type == 1) {
    network_send_poll(zone, seq);          // fire-and-forget
    recv_len = network_recv(response, ...); // non-blocking (SO_NBIO)
    if (recv_len > 2 && response[0] == 0x00) {
        // Server responded — use real data
        memcpy(buf, response + 3, payload_len);
    } else {
        // No server response — return 0x55 magic byte
        memset(buf, 0, max_len);
        ((uint8_t*)buf)[0] = 0x55;
    }
    if (done_cb) { usbd_done_cb_t cb = (usbd_done_cb_t)done_cb; cb(CELL_OK, *len, arg); }
    return CELL_OK;
}
```

### 4. NID Scanner Rewrite (Most Recent)
**Problem:** Scanner used `i += 3` (12-byte triplet stride) assuming `{NID, reserved, GOT_ptr}` format. Official Sony SDK stores NIDs in a flat array (1-word stride) with GOT pointers in a separate array after them. Result: **0/5 hooks found.**

**Fix:** Scanner now uses `i++` (4-byte stride) and searches forward up to 256 words for a valid GOT pointer:

```c
static int get_game_got_slot_range(uint32_t target_nid, uint32_t scan_start,
                                     uint32_t scan_size, ...)
{
    for (i = 0; i < nwords; i++) {           // 1-word stride, not 3-word
        if (words[i] != target_nid) continue;
        for (j = i + 1; j < nwords && j < i + 256; j++) {
            uint32_t candidate = words[j];
            if (candidate < 0x00010000 || candidate > 0x2FFFFFFF) continue;
            volatile uint32_t *got_slot = (volatile uint32_t*)(uintptr_t)candidate;
            uint32_t slot_value = *got_slot;
            if (slot_value == 0 || slot_value > 0x4FFFFFFF) continue;
            *out_got_slot_addr = candidate;
            *out_plt_addr = slot_value;
            return 0;  // Found!
        }
    }
    return -1;
}
```

Two-pass scan: game `.rodata` (0x00100000-0x00B00000) first, then `libusbd.sprx` via `sys_prx_get_module_id_by_name`:

```c
static int get_game_got_slot(uint32_t target_nid, ...)
{
    if (get_game_got_slot_range(target_nid, NID_SCAN_START, NID_SCAN_SIZE, ...) == 0)
        return 0;  // Found in game .rodata

    sys_prx_id_t libusbd_id = sys_prx_get_module_id_by_name("libusbd.sprx", 0, NULL);
    if (libusbd_id >= 0) {
        sys_prx_module_info_t info;
        sys_prx_get_module_info(libusbd_id, 0, &info);
        uint32_t base = (uint32_t)info.segments[0].base;
        uint32_t size = (uint32_t)info.segments[0].filesz;
        if (get_game_got_slot_range(target_nid, base, size & 0xFFFFF, ...) == 0)
            return 0;  // Found in libusbd.sprx
    }
    return -1;  // Not found in either
}
```

### 5. Boot-Time Auto-Load Fixed
**Problem:** Cobra CFW auto-loaded old SPRX at boot via `ldtoypad.enable` file (artifacts from EBOOT patching experiments). PS3MAPI injection conflicted with already-loaded old module.

**Fix:** Deleted `ldtoypad.enable` and `ldtoypad.fake.self` from `/dev_hdd0/plugins/`.

### 6. Logging Improvements
- `papertrail` added to `my_cellUsbdOpenPipe` (low-frequency, safe)
- `papertrail` added to `my_cellUsbdGetDeviceDescriptor` (called once per USB plug)
- One-time `papertrail` on first `InterruptTransfer` call (static counter)
- Actual hook count logged: `[USB] GOT: X/5 hooks installed`
- Build timestamp: `BUILD 2026-07-27-1100` in `module_start`

---

## Verified: Analyze EBOOT Findings

`analyze_eboot.py` scan of the game's EBOOT.BIN:

| NID | Function | Found in EBOOT? |
|-----|----------|-----------------|
| `0x7F5F00D3` | `cellUsbdInit` | ❌ |
| `0x1AB6D80B` | `cellUsbdOpenPipe` | ❌ |
| `0x7B4436CE` | `cellUsbdInterruptTransfer` | ❌ |
| `0x2F82F1A5` | `cellUsbdClosePipe` | ❌ |
| `0x9C8426F7` | `cellUsbdGetDeviceDescriptor` | ❌ |
| `0x3219460D` | `cellUsbdControlTransfer` | ✅ (at 0x007AC8DC) |

Only `cellUsbdControlTransfer` is statically imported by the game. The other 5 NIDs must be in `libusbd.sprx`'s import table. Our dual-pass scanner (game .rodata → libusbd.sprx) should find them.

---

## Questions for Expert

### 1. Scanner Strategy
The new 1-word-stride forward-search scanner finds a NID and then searches the next 256 words for a valid GOT pointer. Is this approach sound for the official Sony SDK import table format? Are there edge cases where the forward search could find the WRONG GOT pointer (e.g., a different function's GOT slot)?

### 2. GOT Pointer Range
We filter GOT candidates to `0x00010000-0x2FFFFFFF`. Is this range correct? The game's GOT is typically in the 0x00800000-0x02000000 range. Could GOT slots exist outside this range?

### 3. libusbd.sprx Name
We look up `libusbd.sprx` via `sys_prx_get_module_id_by_name("libusbd.sprx", 0, NULL)`. Is the module name guaranteed to be `"libusbd.sprx"` on CellOS, or could it be a different name (e.g., `"libusbd"` or `"cellUsbd"`)?

### 4. ControlTransfer NID Already Found
Since `cellUsbdControlTransfer` IS in the game's EBOOT import table, the new scanner should find it and its GOT pointer. But `analyze_eboot.py` located the NID at offset 0x007AC8DC — is the forward search likely to find a valid GOT pointer within 256 words from there?

### 5. Next Steps If Scanner Still Fails
If the next test still shows 0/5 hooks, what alternative approach should we try?
- Parse the SCE stub header to get `nfunc` count and use exact NID→GOT mapping?
- Use `cellUsbdControlTransfer`'s already-known GOT slot to infer the GOT array base and index other functions?
- Bypass NID scanning entirely and hook at the libusbd API level?

### 6. Server-Side Verification
The server (`ld-toypad-server`, UDP 28472) is running. If hooks ARE installed and firing, the server should receive `NET_PACKET_TYPE_POLL` (0x01) packets when the game polls. Is there server-side logging we can enable to confirm data flow before trying to fix the game-side protocol?

## Environment

| Item | Value |
|------|-------|
| PS3 | CECH-2501A, Cobra CFW 4.91, webMAN MOD 1.47.48q |
| PS3 IP | 192.168.0.22 |
| Game | LEGO Dimensions BLUS31473, PID 0x1010200 |
| SDK | Sony DUPLEX SDK 3.40, ppu-lv2-gcc |
| SPRX | /dev_hdd0/plugins/ldtoypad.sprx (24,272 bytes) |
| Injection | PS3MAPI at T+60s |
| PC Server | ld-toypad-server, UDP 28472 at 192.168.0.17 |
| boot_plugins.txt | Only webftp_server.sprx (clean) |
| EBOOT | Verified clean (SHA256: 2378019D...) |
| PSL1GHT artifacts | None — verified all headers from official SDK |
