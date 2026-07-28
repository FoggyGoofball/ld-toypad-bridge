# Expert Questions — 2026-07-27: Stuck at Connect ToyPad (Hooks Installed)

## Current State

All 5 hooks install successfully via unconditional NID-based GOT overwrite:
- `cellUsbdOpenPipe` (offset 0)
- `cellUsbdInterruptTransfer` (offset 64) — non-blocking, polls server via `network_send_poll` + `network_recv` (socket is SO_NBIO)
- `cellUsbdClosePipe` (offset 128)
- `cellUsbdGetDeviceDescriptor` (offset 192) — returns ToyPad VID/PID
- `cellUsbdControlTransfer` (offset 256) — returns fake HID descriptors
- Heartbeat at offset 320

Boot log confirms: `"OK: usb_hook_init() — 5 USB hooks INSTALLED (Trojan Horse ready!)"`

Server is running (UDP 28472). No visual corruption. Game stays at "Connect ToyPad" screen after USB drive plugged in.

---

## Questions

### 1. Hook Verification: Installed vs. Firing

The hooks are INSTALLED (GOT slots overwritten, boot log confirms). But are they actually being CALLED? The GOT overwrite replaces the function pointer — but could the game be calling these functions through a different path (e.g., through `libusbd.sprx` internal calls that bypass the GOT)?

How can we verify the hooks are firing? Would adding a single `papertrail()` call inside `my_cellUsbdOpenPipe` (called only once or twice, not high-frequency like InterruptTransfer) be safe for confirming the attach sequence started?

### 2. Game State: Connect ToyPad Screen

Does LEGO Dimensions transition to a DIFFERENT screen once it detects a valid USB HID device? Or does it stay on "Connect ToyPad" indefinitely until it receives specific tag data (character placed on pad)?

In other words: if the game detects the ToyPad but no characters are placed, is the expected behavior:
- A) "Connect ToyPad" screen → "Place a figure" screen (different screen)
- B) "Connect ToyPad" screen stays, but with a different message
- C) "Connect ToyPad" screen stays identical until tag data arrives

If the answer is (A) and we're stuck at "Connect ToyPad," the game never transitioned — meaning our hooks aren't providing the expected attach handshake sequence.

### 3. USB HID Initialization Sequence

After the USB control transfers complete (descriptors obtained, configuration set), what HID command sequence does LEGO Dimensions send to initialize the ToyPad?

Does it:
1. Send an OUT command with a specific initialization byte?
2. Expect a specific IN response (firmware version, status, etc.) before starting normal polling?
3. Poll repeatedly until it gets a non-empty tag response?

If there's a specific initialization handshake we're not handling, that would explain why the game stays stuck.

### 4. Server Connectivity

The server is at `192.168.0.17:28472`. The PS3 is at `192.168.0.22`. The SPRX binds to port 28472 on the PS3 and sends to the server. Is the server receiving the poll packets (`NET_PACKET_TYPE_POLL` = 0x01)? 

If the server isn't receiving polls, the game sends OUT data that the server never processes, and the server never sends IN responses. The game would be polling and getting empty "no tag" responses forever.

Should we add a server-side log that confirms when it receives a poll from the PS3?

### 5. OpenPipe Verification

Should we add a single `papertrail()` call inside `my_cellUsbdOpenPipe`? This hook fires at most 2-3 times (once for each endpoint). The expert previously advised against logging in high-frequency hooks, but OpenPipe is low-frequency. Confirming OpenPipe fires would tell us:
- ✅ OpenPipe fired → game attached to device, opened pipes → issue is in InterruptTransfer
- ❌ OpenPipe never fired → game never called attach → issue is earlier in the chain

## Environment

| Item | Value |
|------|-------|
| PS3 | CECH-2501A, Cobra CFW 4.91, webMAN MOD 1.47.48q |
| PS3 IP | 192.168.0.22 |
| Game | LEGO Dimensions BLUS31473 |
| SDK | Sony DUPLEX SDK 3.40, ppu-lv2-gcc |
| SPRX | /dev_hdd0/plugins/ldtoypad.sprx (23,456 bytes) |
| Injection | PS3MAPI at T+60s |
| PC Server | ld-toypad-server, UDP port 28472 at 192.168.0.17 |
| Hooks | 5/5 installed via NID scan (unconditional GOT overwrite) |
| boot_plugins.txt | Only webftp_server.sprx (clean) |
