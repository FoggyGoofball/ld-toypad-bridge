# LD-ToyPad Bridge

Network bridge for **LEGO® Dimensions Toy Pad** emulation on **PS3 CFW (Evilnat 4.93 CEX)**.

This project replaces the physical Toy Pad USB peripheral with a PC-based emulator, enabling
remote toy placement via a web UI while the PS3 runs the LEGO Dimensions game unmodified.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  PC (Windows)                                                    │
│                                                                   │
│  ┌──────────────────────┐     UDP broadcast beacons (every 1s)    │
│  │   Node.js Server     │ ──────────────────────────────────────┐ │
│  │   (server.js)        │                                       │ │
│  │                      │ ◄── UDP Poll/Read/Write packets ───┐ │ │
│  │  ┌────────────────┐  │                                     │ │ │
│  │  │ HTTP API :8080 │  │    ┌────────────────────────────┐   │ │ │
│  │  │ Web UI        │  │    │ UDP Debug Logs :28473      │   │ │ │
│  │  └────────────────┘  │    │ Optional remote debugging  │   │ │ │
│  └──────────────────────┘    └────────────────────────────┘   │ │ │
│           │                                                    │ │ │
│  ┌──────────────────────┐                                      │ │ │
│  │  Launcher Scripts    │                                      │ │ │
│  │  start-ldtoypad.bat  │                                      │ │ │
│  │  run-bridge-elevated │                                      │ │ │
│  └──────────────────────┘                                      │ │ │
└────────────────────────────────────────────────────────────────┘ │ │
                                                                   │ │
┌─────────────────────────────────────────────────────────────────┐ │ │
│  PS3 (Evilnat 4.93 CEX)                                         │ │ │
│                                                                  │ │ │
│  ┌──────────────────────────┐                                    │ │ │
│  │ /dev_hdd0/plugins/       │                                    │ │ │
│  │   ldtoypad.sprx          │◄── Loaded at boot (if enabled)    │ │ │
│  │   ldtoypad.enable        │                                    │ │ │
│  │   ldtoypad.fake.self     │                                    │ │ │
│  └──────────────────────────┘                                    │ │ │
│           │                                                     │ │ │
│   [USB Hook Engine]  ───  Intercepts sys_usbd calls             │ │ │
│           │                for Toy Pad device                    │ │ │
│           ▼                                                     │ │ │
│   [Toypad State]  ───────  Spoofs USB descriptors              │ │ │
│                             + routes HID over UDP                │ │ │
│                                                                  │ │ │
│   Startup: network_init() sends beacon ─────────────────────────┘ │ │
│            then 3-second recv spin for server response ──────────┘ │
│   Runtime: USB interrupt handler calls network_maybe_probe_server()│
│            + network_send_poll() on each game poll cycle           │
└────────────────────────────────────────────────────────────────────┘
```

## Repository Structure

```
ld-toypad-bridge/
├── ld-toypad-server/          # Node.js bridge server (runs on Windows PC)
│   ├── server.js              # UDP server + HTTP API main entry point
│   ├── toypad-protocol.js     # USB HID protocol constants & helpers
│   ├── virtual-toys.js        # Virtual toy database + zone manager
│   ├── image-manifest.js      # Wiki image sync support
│   ├── run-bridge-elevated.bat# Elevated launcher with firewall rules
│   ├── web/                   # Browser UI (HTML/CSS/JS)
│   │   ├── index.html
│   │   ├── app.js
│   │   └── styles.css
│   ├── data/                  # Toy catalog JSON (from LD-ToyPad-Emulator)
│   │   ├── charactermap.json
│   │   └── tokenmap.json
│   ├── scripts/               # Utilities
│   │   ├── sync-fandom-images.js
│   │   └── query-ps3-syscalls.js
│   ├── images/                # Toy thumbnails (optional, user-provided)
│   └── package.json
│
├── sprx-plugin/               # PS3 SPRX plugin (C, PSL1GHT SDK)
│   ├── main.c                 # Entry point, enable flag check, hook install
│   ├── network.c              # UDP transport, startup beacon + recv spin
│   ├── network.h
│   ├── usb_hooks.c            # LV2 syscall table patching engine
│   ├── usb_hooks.h
│   ├── toypad_state.c         # USB descriptor spoofing, interrupt handlers
│   ├── toypad_state.h
│   ├── debug.c                # Formatted logging + UDP remote log stream
│   ├── debug.h
│   ├── compat.c               # libc-light memory/string helpers
│   ├── Makefile               # Cross-compile for PSL1GHT
│   ├── build.sh               # WSL build helper
│   ├── fix_psl1ght.sh         # SDK workarounds
│   └── fix_sources.sh
│
├── start-ldtoypad.bat         # Quick-start launcher (non-elevated)
├── docs/                      # Handoff & changelog documentation
├── ps3-ip.txt                 # Cached PS3 IP address
├── ftp-deploy.ps1             # PowerShell: deploy SPRX to PS3
├── ftp-deploy.ps1             # PowerShell: upload verify
├── deploy-enable.ps1          # PowerShell: create enable flag on PS3
└── .gitignore
```

## Quick Start (Server Only)

```bash
cd ld-toypad-server
npm install
node server.js --ps3-ip 192.168.0.47
```

Then open **http://localhost:8080** for the web UI.

### Launcher Script (Windows)

Double-click `start-ldtoypad.bat` — it prompts for elevation, kills stale bridge processes,
waits for PS3 ping, and launches the server.

## Building the SPRX Plugin

The SPRX must be cross-compiled using **PSL1GHT** toolchain on **WSL (Ubuntu)**:

```bash
# On WSL, from the PSL1GHT SDK environment:
cd /path/to/sprx-plugin
make clean 2>/dev/null || true
make all 2>&1
```

Output: `build/ldtoypad.sprx` (and `build/ldtoypad.fake.self`)

## Deploying to PS3

1. Upload the SPRX via FTP to `/dev_hdd0/plugins/ldtoypad.sprx`
2. Create the enable flag: `/dev_hdd0/plugins/ldtoypad.enable` (content: "enabled")
3. Optionally upload `build/ldtoypad.fake.self` for webMAN compatibility
4. Reboot the PS3 — the SPRX should load during boot

### Enable Flag

The plugin checks for `/dev_hdd0/plugins/ldtoypad.enable` at startup.
Without this file, the plugin stays dormant (returns `SYS_PRX_START_OK` but does nothing).

## How Discovery Works

1. **PS3 boots** → SPRX `_start()` runs → `network_init()` opens UDP socket
2. SPRX sends a startup beacon, then enters a **3-second recv spin** looking for server broadcasts
3. **PC server** sends periodic broadcast beacons every 1 second (dual: both `255.255.255.255` broadcast AND directed to cached PS3 IP)
4. If the 3-second spin catches a server beacon → `g_net.server_known` is set → discovery complete
5. **Fallback**: If the server starts after the PS3, discovery happens via `network_maybe_probe_server()` during USB interrupt polling (game must be running)

## Network Protocol

### PS3 → PC (inbound, 8-byte header)

| Offset | Size | Description |
|--------|------|-------------|
| 0 | 1 | Packet type (0x01=Poll, 0x02=ReadTag, 0x03=WriteTag, 0x04=DataOut) |
| 1 | 1 | Zone (0=LEFT, 1=CENTER, 2=RIGHT) |
| 2 | 1 | Sequence number |
| 3-7 | 5 | Reserved |

### PC → PS3 (outbound, 80 bytes)

| Offset | Size | Description |
|--------|------|-------------|
| 0 | 1 | Status (0x00=OK, 0x01=NoTag, 0xFF=Error) |
| 1 | 1 | Zone (echoed) |
| 2 | 1 | Sequence number (echoed) |
| 3-79 | 77 | Response data (USB HID report payload) |

## Current Status

✅ **Server**: Fully functional — web UI, HTTP API, multi-slot zones (L=3, C=1, R=3),
   virtual toy database (322 entries), portal telemetry, keystone glow visualization.

❌ **SPRX not loading on PS3** — The plugin files are deployed and confirmed present
   on the PS3 via FTP, but zero UDP packets have been received from the PS3 after
   multiple reboots with the server broadcasting. The CFW plugin loading mechanism
   on **Evilnat 4.93 CEX** may not auto-load from `/dev_hdd0/plugins/`, or the
   syscall table address `0x800000000000F300` may be incorrect, or the SPRX format
   may need adjustments.

### Key Blockers

1. **SPRX auto-load on Evilnat 4.93** — investigate CFW plugin path requirements
2. **LV2 syscall table address** — confirm `0x800000000000F300` for Evilnat 4.93 CEX
3. **Firmware-specific LV2 key** — currently placeholder in `usb_hooks.c`
4. **Write protection disable/enable** — currently stubs in `usb_hooks.c`

See **[docs/CHANGELOG-HANDOFF-2026-07-14.md](docs/CHANGELOG-HANDOFF-2026-07-14.md)** for full
verification results, live test data, and prioritized next steps.

## Credits

- **[Berny23/LD-ToyPad-Emulator](https://github.com/Berny23/LD-ToyPad-Emulator)** — Original Toy Pad USB HID protocol reverse engineering and tag data
- **[PS3XPAD](https://github.com/PS3X-Dev/PS3Xpad)** — LV2 syscall patching reference
- **PSL1GHT** — PS3 open source SDK

## License

This project is for educational/research purposes. LEGO Dimensions is a trademark of
The LEGO Group and Warner Bros. Interactive Entertainment. No game assets are included.
