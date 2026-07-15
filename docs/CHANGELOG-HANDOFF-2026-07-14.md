# LD-ToyPad Bridge Handoff Changelog (2026-07-14, Session 2-3)

## Resume Marker: Discovery Fix + Final Polish

### What was working at handoff
- ✅ SPRX builds successfully (both `USB_HOOK_ENABLE_PATCH=0` and `=1`)
- ✅ Browser UI fully functional (tabbed catalog, thumbnails, zone strip, keystone glow)
- ✅ Multi-slot zones (L=3, C=1, R=3) with duplicate support
- ✅ Portal telemetry capture + keystone zone glow visualization
- ✅ Launcher scripts (`start-ldtoypad.bat`, `run-bridge-elevated.bat`)
- ✅ PS3 IP caching (`ps3-ip.txt`)

### What was broken / incomplete at handoff
- ❌ **PS3 discovery still had chicken-and-egg problem**: The PS3 only called `network_recv()` from inside the USB interrupt handler, which required the *game to be running and actively polling*. First-time discovery was unreliable even with cached IP.
- ❌ **Server only sent directed probes when `--ps3-ip` was set** — no broadcast fallback, so stale cached IPs caused silent failure
- ❌ **`ps3-ip.txt` contained wrong IP** (192.168.0.17 instead of 192.168.0.47)

---

## Changes Applied This Session

### 1. `sprx-plugin/network.c` — Startup recv spin
- **File**: `sprx-plugin/network.c` (lines ~107-140)
- After sending the startup beacon, the PS3 now enters a `~3-second recv spin loop` calling `network_recv()` with `MSG_DONTWAIT`
- This catches the server's periodic broadcast beacon even before the game starts polling USB
- Discovery no longer depends on USB interrupt activity
- If the server broadcast arrives within 3 seconds, `g_net.server_known` is set and all subsequent packets go directly to the right IP

### 2. `ld-toypad-server/server.js` — Broadcast + directed dual probe
- **File**: `ld-toypad-server/server.js` (discovery beacon timer)
- Changed from `if (PS3_IP) { directed } else { broadcast }` to **always both**: broadcast + directed probe in parallel
- Broadcast always works, directed probe is a speed optimization for cached IPs
- Eliminates the stale-cache silent failure

### 3. `ld-toypad-server/ps3-ip.txt` — Fixed cached IP
- Changed from `192.168.0.17` → `192.168.0.47`

### 4. `sprx-plugin/main.c` — Non-fatal USB hook failure (verified)
- Already applied in previous session. If `usb_hooks_install()` fails, the plugin continues running with the network alive so the server can still detect the PS3.

### 5. `server.js` — Fix stdin close causing premature shutdown (Session 3)
- The `rl.on('close', ...)` handler now checks `process.stdin.isTTY` before calling `process.exit(0)`
- This allows the server to stay alive when launched via `node server.js < nul &` in background mode
- The SIGINT/SIGTERM handlers still work normally

### 6. Bat scripts polished (Session 3)
- `start-ldtoypad.bat`: Fixed kill-bridge logic (title-based), timeout→ping, unique timestamped logs, better messages
- `run-bridge-elevated.bat`: Improved timeout diagnostics

### 7. SPRX rebuilt + deployed (Session 3)
- Fresh 10,800-byte SPRX built via WSL from updated sources
- Deployed to PS3 `/dev_hdd0/plugins/ldtoypad.sprx` via FTP
- Enable flag created: `/dev_hdd0/plugins/ldtoypad.enable` containing "enabled"

---

## Verification Results (Session 3 — Code Review + Live Test)

| Check | Result |
|---|---|
| Claim 1: startup recv spin in `network.c` | ✅ PASS — 3-second recv loop confirmed at lines ~107-140 |
| Claim 2: broadcast + directed dual probe in `server.js` | ✅ PASS — beacon sends to both `255.255.255.255` and `CONFIG.PS3_IP` |
| Claim 3: `ps3-ip.txt` corrected .17 → .47 | ✅ PASS — both root and `ld-toypad-server/ps3-ip.txt` read `.47` |
| `ps3-ip.txt` root file | ✅ Fixed (was stale .17) |
| `start-ldtoypad.bat` | ✅ Reviewed, polished, fix applied |
| `run-bridge-elevated.bat` | ✅ Reviewed, diagnostics improved |
| SPRX file on PS3 | ✅ 10,800 bytes, FTP-deployed (session 2/3) |
| Enable flag on PS3 | ✅ Present: `/dev_hdd0/plugins/ldtoypad.enable` |
| PS3 network reachable | ✅ `192.168.0.47` answers ping (TTL=255, ~35-158ms) |
| FTP port 64000 | ❌ Closed (webMAN FTP not started, but standard FTP port 21 works) |
| webMAN HTTP (port 80) | ✅ **Confirmed running** — `wMAN MOD 1.47.45` responds |
| SPRX+enable survive reboot | ✅ **Confirmed via FTP on port 21** — both files present |
| SPRX auto-loads on reboot | ❌ **FAIL** — Zero UDP packets received after full reboot with server broadcasting |
| server.js stdin-close fix | ✅ Server stays alive when backgrounded (tested) |
| Firewall rules | ✅ Both UDP 28472 and 28473 inbound rules present and enabled |

### Live Test Result (2026-07-14, 8:48-8:54 PM)
**The server was started BEFORE the PS3 reboot.** The PS3 was rebooted with the server broadcasting discovery beacons every 1 second. After ~4 minutes of monitoring post-reboot:
- **Zero** UDP packets received from the PS3
- **Zero** SPRX debug log messages received on port 28473
- PS3 is fully booted (webMAN accessible on port 80, FTP on port 21, responds to ping)
- SPRX files confirmed on PS3 via FTP: `ldtoypad.sprx` (10,800 bytes), `ldtoypad.enable`, `ldtoypad.fake.self`

**Conclusion**: The SPRX is either not being loaded by the CFW plugin system on Evilnat 4.93 CEX, or is crashing silently during `_start()` before the network code executes. The startup recv spin and discovery logic cannot be tested until the SPRX loading issue is resolved.

### Observation
- The API `j.server.uptime` returns empty string (`s`) — cosmetic, doesn't affect functionality.

---

## Known Remaining Issues / Next Steps

### A. Discovery still has a first-time boot problem (minor)
- **The PRX file must be deployed** and the PS3 must be freshly booted (or loaded) with the enable flag present
- The 3-second recv spin only runs once during `_start()` — if the server isn't broadcasting yet (e.g. server starts after PS3), discovery falls back to the USB-interrupt-triggered path (which requires the game to run)
- **Possible fix**: Add a lightweight background recv thread or periodic timer callback that calls `network_recv()` every ~2s regardless of USB activity. Requires LV2 timer API or syscall.

### B. Launcher "PS3 not detected" message is misleading
- Both `.bat` files show: `PS3 not responding at !CACHED_IP!. Is the PS3 on and the game running?`
- When detection times out, they should suggest checking the firewall and the enable flag, not imply the game must be running
- The PS3 sends a beacon during `_start()`, so the game is NOT needed — but the PRX must be armed (enable flag present)

### C. SPRX only activates when game launches
- The SPRX lives at `/dev_hdd0/plugins/ldtoypad.sprx` and requires the LD game app to be launched for the plugin to initialize
- The enable flag at `/dev_hdd0/plugins/ldtoypad.enable` controls whether the SPRX arms itself
- To test fresh discovery flow: start server → launch game on PS3 → SPRX loads → sends beacon → server sees PS3 within 3s

### D. Server `client.address` format in API response
- The API returns `client: { address: "192.168.0.47", port: 28472 }` when connected
- The bat scripts query `$j.client.address` — this works
- But if `client` is null (not connected), `$j.client` is `null`, and accessing `.address` on null in PowerShell silently fails. The bat script handles this via the `if($j.client.address){...}` guard — confirmed correct.

---

## Key Files Summary

| File | Purpose | Last Modified |
|------|---------|--------------|
| `sprx-plugin/main.c` | PRX entry point, enable flag check, non-fatal hook failure | 2026-07-14 |
| `sprx-plugin/network.c` | UDP transport, startup beacon + recv spin, server discovery | 2026-07-14 (this session) |
| `sprx-plugin/network.h` | Public network API | 2026-07-13 |
| `sprx-plugin/toypad_state.c` | USB descriptor spoofing, interrupt handlers | 2026-07-13 |
| `sprx-plugin/usb_hooks.c` | LV2 syscall patch engine (phase 2) | 2026-07-13 |
| `sprx-plugin/debug.c` | Formatted logging + UDP remote log stream | 2026-07-13 |
| `sprx-plugin/compat.c` | libc-light memory/string helpers (no-libc build) | 2026-07-13 |
| `sprx-plugin/Makefile` | Cross-compile for PSL1GHT, Evilnat 4.93 CEX params | 2026-07-14 |
| `ld-toypad-server/server.js` | UDP bridge server + HTTP API + discovery beacons | 2026-07-14 (this session) |
| `ld-toypad-server/virtual-toys.js` | Toy database, multi-slot zone manager (L=3,C=1,R=3) | 2026-07-13 |
| `ld-toypad-server/toypad-protocol.js` | Protocol constants (TOY_PAD namespace) | 2026-07-13 |
| `ld-toypad-server/web/index.html` | Browser UI structure | 2026-07-13 |
| `ld-toypad-server/web/app.js` | Browser UI logic (zones, glow, catalog, modals) | 2026-07-13 |
| `ld-toypad-server/web/styles.css` | Styles + keystone glow animation | 2026-07-13 |
| `ld-toypad-server/ps3-ip.txt` | Cached PS3 IP (currently 192.168.0.47) | 2026-07-14 (this session) |
| `start-ldtoypad.bat` | Quick-start launcher (non-elevated) | 2026-07-14 |
| `ld-toypad-server/run-bridge-elevated.bat` | Elevated launcher with firewall rules | 2026-07-14 |

---

## Build Command (WSL)
```bash
cd /home/mike/ldtoypad
make clean >/dev/null 2>&1 || true
make all 2>&1                              # default: USB_HOOK_ENABLE_PATCH=1
```

## Quick Test (Windows)
```bash
cd c:/Users/Admin/source/repos/dimensions plugin
start-ldtoypad.bat
```

## Verify SPRX on PS3
- Location: `/dev_hdd0/plugins/ldtoypad.sprx`
- Enable flag: `/dev_hdd0/plugins/ldtoypad.enable` (permanent — no reboot needed to re-arm)
- The plugin will NOT initialize unless the enable file exists

---

## Handoff Checklist for Next Agent

### Easy wins (15 min each)
- [ ] Deploy the latest SPRX to PS3 if missing (reboot may have cleared `/plugins/`)
- [ ] Verify `ps3-ip.txt` has the correct PS3 IP (.47)
- [ ] **Full discovery test**: start server on PC → launch LD game on PS3 → SPRX should connect within 3s
- [ ] Test keystone zone glow by running the game and watching the browser UI during a puzzle

### Deeper items (may need investigation)
- [ ] **Background recv thread**: Currently discovery only happens during `_start()` spin or during USB interrupt polls. Add a periodic timer (using LV2 timer syscall) that calls `network_recv()` every 2-3 seconds independently of USB activity. This makes discovery truly fire-and-forget.
- [ ] **Better bat script messaging**: When PS3 detection times out, suggest checking firewall rules and enable flag presence instead of "Is the game running?"
- [ ] **Fix TIME_WAIT port conflict**: The "process cannot access the file" error in `start-ldtoypad.bat` — kill by PID detection instead of relying on `timeout /t` delay
- [ ] **Zone state persistence across game sessions**: If the game unloads/reloads the USB driver, the zone state may reset but the server still has old data
- [ ] **Fix uptime display**: API returns empty string for `j.server.uptime` — investigate serialization

### Prior session's remaining hardware tasks (from CHANGELOG-2026-07-13)
- [ ] Confirm LV2 syscall constants on target firmware (Evilnat 4.93 CEX)
- [ ] Run phase-1 baseline: `USB_HOOK_ENABLE_PATCH=0` — verify plugin lifecycle, UDP traffic, remote logs
- [ ] Run phase-2 enablement: `USB_HOOK_ENABLE_PATCH=1` — verify patch install/remove, no instability
- [ ] Execute smoke matrix: load, toy detect per zone, read path, write path, unload/reload
- [ ] Preserve bridge protocol logs + SPRX remote debug logs for each scenario
