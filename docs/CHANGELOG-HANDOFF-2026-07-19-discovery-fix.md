# CHANGELOG / HANDOFF — Discovery Protocol Fix
## Date: 2026-07-19 19:57 UTC-6

### Root Cause
Two critical bugs prevented discovery completion between the PS3 SPRX plugin and the PC server:

**1. Self-Discovery Loop (C code — network.c)**
When the PS3 sends a UDP broadcast to 192.168.0.255:28472, the FreeBSD kernel mirrors it back to any local socket listening on port 28472. The original `network_recv()` blindly accepted the **first** packet it received — which was its own broadcast — locking `g_net.server` to 192.168.0.47 (itself) and halting further discovery.

**2. Protocol Packet Type Collision (C code + server.js)**
Both sides were using `0x01` for different purposes:
- PS3 sent `0x01` as a discovery probe (`NET_PACKET_TYPE_POLL`)
- Server recognized `0x01` as `PACKET_TYPE_POLL` (Toy Pad state poll request)
- If the server did receive the PS3's probe, it would interpret it as a state request and send back a malformed response

### Changes Made

#### network.h
- Added `NET_PACKET_TYPE_DISCOVERY  0xF0` — a distinct magic byte for discovery packets
- Added `uint32_t self_ip` field to `struct net_state` to store the PS3's own IP

#### network.c
- **Self-IP detection** (`network_wait_ready`): After the 3s interface readiness wait, calls `getsockname()` to retrieve the PS3's assigned IP address. Writes to `/dev_hdd0/tmp/ld_self_ip.txt` for papertrail validation.
- **Self-rejection** (`network_recv`): Before processing any received packet, compares `from_addr.sin_addr.s_addr` against `g_net.self_ip`. If they match (self-looped broadcast), returns 0 immediately.
- **Magic byte filter** (`network_recv`): When `g_net.server_known` is false, only accepts packets where `buffer[0] == NET_PACKET_TYPE_DISCOVERY` (0xF0). All other packet types are silently ignored during discovery.
- **Discovery probes** (`network_maybe_probe_server`): Now sends `NET_PACKET_TYPE_DISCOVERY` (0xF0) as the first byte instead of `NET_PACKET_TYPE_POLL` (0x01).
- **Startup beacon salvo** (`network_wait_ready`): Uses 0xF0 for all 10 rapid-fire beacons.
- **Papertrail logging** (`network_recv`): First 10 received packets dump sender IP, port, type byte, and length to `/dev_hdd0/tmp/ld_recv_papertrail.txt`.

#### server.js
- Added `PACKET_TYPE_DISCOVERY: 0xF0` to config constants.
- **Discovery-only client registration**: The client registration filter now **requires** `msg[0] === CONFIG.PACKET_TYPE_DISCOVERY` before accepting a `clientAddress`. This prevents protocol collision where a stray 0x01 poll from the PS3 could register before the server's discovery response.
- **Beacon uses 0xF0**: The broadcast beacon now sends `PACKET_TYPE_DISCOVERY` in byte 0 instead of `RESPONSE_NO_TAG`.
- **Discovery ACK handler**: New `case CONFIG.PACKET_TYPE_DISCOVERY` in `processPacket()` calls `buildDiscoveryAck()`, which echoes back 0xF0 as acknowledgment.
- Added `buildDiscoveryAck()` function.

### Testing Protocol
1. Start PC server: `cd ld-toypad-server && node server.js --host 0.0.0.0 --http-port 8080 --port 28472 --debug-port 28473 --ps3-ip 192.168.0.47 --verbose`
2. Check `http://localhost:8080/api/status` — `client` should be `null`
3. Reboot PS3
4. Watch for `[Server] Client connected from 192.168.0.47:28472`

### Files Modified
- `sprx-plugin/network.h` — Added DISCOVERY constant + self_ip struct field
- `sprx-plugin/network.c` — Self-rejection, magic byte filtering, papertrail
- `ld-toypad-server/server.js` — Discovery beacon, client registration filter, ACK handler

### Author
Expert analysis from handoff report; implementation by Cline AI.
