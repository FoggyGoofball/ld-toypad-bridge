# LD-ToyPad Bridge — Diagnostics Guide (2026-07-22)

## Three-Layer Diagnostic Architecture

This project uses a **3-layer diagnostic approach** to isolate connectivity issues:

```
Layer 1: Local PC Loopback Test
Layer 2: WSL2/VM-to-Host Test  
Layer 3: Real PS3-to-PC Test
```

---

## Layer 1: Local PC Loopback Test

**Purpose:** Verify the server is running and correctly processing packets **without touching any network**.

**On the PC:**
```powershell
# Terminal 1: Start the server
node ld-toypad-server\server.js --verbose

# Terminal 2: Run the simulator (sends discovery beacon 0xF0)
node ld-toypad-server\simulate-ps3.js
```

**Expected output in Terminal 1:**
```
[Server] RX from 127.0.0.1:xxxxx type=0xf0 len=8 hex=...
[Server] Client connected from 127.0.0.1:xxxxx
```

**If you see this — your server is working correctly at the code level.**

**If not:**
- Is the server actually running? (check port 28472 with `netstat -an | findstr 28472`)
- Is `node simulate-ps3.js` in the right directory?
- Can you reach `http://localhost:8080/api/status` in a browser?

---

## Layer 2: WSL2/VM-to-Host Test (if applicable)

**Purpose:** Verify that WSL2 (where the injection scripts may run) can reach the Windows host.

```bash
# From inside WSL2:
node /mnt/c/.../ld-toypad-server/simulate-ps3.js --server 192.168.0.17
```

Replace 192.168.0.17 with your actual Windows LAN IP.

**Expected:** Same Layer 1 output, but showing the WSL2 IP instead of 127.0.0.1.

---

## Layer 3: Real PS3-to-PC Test

**Purpose:** Verify the PS3 can actually deliver UDP packets to the PC.

### Step A: Confirm PC IP
```powershell
ipconfig
```
Look for your LAN adapter IPv4 address (e.g. 192.168.0.17, 192.168.0.100, etc.)

### Step B: Update PS3 IP
Edit `sprx-plugin/main.c` line 217:
```c
network_set_server(htonl(0xC0A80011), 28472);
```
Replace `0xC0A80011` (192.168.0.17) with your actual PC IP in hex:
- 192.168.0.17   → 0xC0A80011
- 192.168.0.100  → 0xC0A86464
- 10.0.0.5       → 0x0A000005
- 172.16.1.50    → 0xAC100132

### Step C: Test Firewall
```powershell
# Check if Windows Firewall is blocking port 28472:
Test-NetConnection -ComputerName localhost -Port 28472 -InformationLevel Detailed

# Create allow rule (run as Administrator):
New-NetFirewallRule -DisplayName "LD-ToyPad UDP 28472" `
  -Direction Inbound -Protocol UDP -LocalPort 28472 -Action Allow
New-NetFirewallRule -DisplayName "LD-ToyPad UDP 28472 OUT" `
  -Direction Outbound -Protocol UDP -LocalPort 28472 -Action Allow
```

### Step D: Verify on PS3
1. Start server on PC with `--verbose`
2. Inject SPRX into game on PS3
3. In the server logs, you should see:
   ```
   RX from <PS3_IP>:<port> type=0xEE len=8 hex=...
   ```
   This is the keepalive — sent every 3 seconds from the worker loop.

   If you see the keepalive but no discovery (0xF0):
   - Discovery may be blocked by broadcast routing
   - The keepalive proves the hardcoded server IP is reachable

   If you see NOTHING:
   - Firewall blocking inbound UDP
   - Wrong PC IP in main.c
   - PS3 not on the same subnet
   - Some routers block UDP between wired/wireless

---

## Packet Type Reference

| Byte 0 | Name          | Direction     | Description                 |
|--------|---------------|---------------|-----------------------------|
| 0x01   | POLL          | PS3 → PC      | USB HID poll from game      |
| 0x04   | DATA_OUT      | PS3 → PC      | LED data from game to pad   |
| 0xEE   | KEEPALIVE     | PS3 → PC      | Heartbeat (every 3s)        |
| 0xF0   | DISCOVERY     | Both ways     | Broadcast beacon            |

---

## Heartbeat Counter Verification

The SPRX writes to a heartbeat counter at a known offset in the R-W-X trampoline page:

1. **In the game process:** Scan the trampoline page address (logged by SPRX on init)
2. **Read process memory** via PS3MAPI: `download.ps3?addr=0xXXXXXX&size=4`
3. **Check that the value increments** (should change ~20 times per second)

If it increments: SPRX code is running. Problem is network.
If it doesn't: SPRX crashed or the game isn't executing the trampolines.

---

## Common Issues

### "I see clientAddress but no poll/transfer data"
The game may not be calling the hook functions because the preambles haven't been written. The SPRX writes an IPC file (`/dev_hdd0/tmp/ld_hooks_ready.txt`) that Node.js must read to know where to write the jump preambles.

### "Keepalive shows up but no discovery"
The keepalive is sent from the worker loop using the hardcoded server IP. Discovery probes use broadcast (192.168.0.255). If the PS3 is on a different subnet (most home networks are /24, this should work), broadcast might be blocked by the router.

### "Firewall is open but still no packets"
Some routers (especially ISP-provided ones) have "Client Isolation" or "AP Isolation" features that block device-to-device traffic. Check router settings.
