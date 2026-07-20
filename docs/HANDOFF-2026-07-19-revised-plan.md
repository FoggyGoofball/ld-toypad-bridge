# Architecture Revision Report — Step A: Static IP + Protocol Cleanup
## Date: 2026-07-19 20:24 UTC-6

## Background
The original handoff (HANDOFF-2026-07-19-final.md) identified two critical bugs:
1. Self-discovery loop (theorized)
2. Packet type collision (0x01 used for two purposes)

After expert peer review, the following conclusions were reached:
- **Self-loopback is unconfirmed** on PS3 CellOS/FreeBSD and likely not the real blocker
- **The 0x01 byte is standard USB HID Report ID**, not a custom packet type — the original "collision" framing was a layering misunderstanding
- **0xF0 discovery ACK risks payload collision** with `my_cellUsbdTransfer` which checks `response[0] == 0x00`
- **Server's 255.255.255.255 beacon is unreliable** on multi-interface PS3s

## Solution: Step A — Hardcode + Simplify
Bypass all broadcast discovery complexity by hardcoding the dev PC IP:

### Changes Made

#### `sprx-plugin/main.c`
- Added `network_set_server(htonl(0xC0A80011), 28472)` after `network_wait_ready()`
- This hardcodes PC at 192.168.0.17:28472, eliminating ALL broadcast unknowns
- Probe broadcasts in `network_maybe_probe_server` still fire but are short-circuited by `g_net.server_known == 1`

#### `ld-toypad-server/server.js`
- **Removed** `buildDiscoveryAck()` function — was sending 0xF0 in response byte which would corrupt USB HID stream
- **Removed** `case CONFIG.PACKET_TYPE_DISCOVERY` from `processPacket` switch — no longer treat 0xF0 as a special server-handled type
- **Simplified** client registration — removed the `msg[0] === PACKET_TYPE_DISCOVERY` gate. Now accepts any packet from non-local IPs (the self-IP skip still prevents self-registration)
- Kept `PACKET_TYPE_DISCOVERY` constant (still used for server beacon broadcast)
- Kept server beacon sending 0xF0 (harmless, PS3 ignores it when server is known)

#### Preserved from previous changes
- `sprx-plugin/network.h`: `NET_PACKET_TYPE_DISCOVERY` constant, `self_ip` field — harmless
- `sprx-plugin/network.c`: Self-IP detection, self-rejection, papertrail — defensive, zero cost
- `sprx-plugin/network.c`: `network_maybe_probe_server` uses 0xF0 — harmless fallback

## Testing
1. Start PC server: `cd ld-toypad-server && node server.js --host 0.0.0.0 --http-port 8080 --port 28472 --debug-port 28473 --ps3-ip 192.168.0.47 --verbose`
2. Reboot PS3
3. SPRX boots, `network_set_server` sets PC IP immediately, discovery probes are skipped
4. PS3 can send 0x01 (HID Report ID) poll packets to the server immediately
5. Server responds with standard 0x00/0x01 status bytes that `my_cellUsbdTransfer` recognizes
