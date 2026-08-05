# CHANGELOG — LD-ToyPad Bridge: Overlay Fixes, DS3 Pairing & Expert Questions

**Date:** 2026-08-05
**Project:** LEGO Dimensions ToyPad Emulation for PS3
**Session scope:** Overlay UI fixes (1:1 Berny23 parity), cross-analyzer agent, composite gadget analysis, DS3 one-time Bluetooth pairing via FunctionFS

---

## Part 1: Overlay UI — 1:1 Berny23 Parity Fixes

### Rationale

The custom overlay (`deck/overlay/`) was a ground-up vanilla JS rewrite of Berny23's jQuery UI. Cross-analysis against the upstream revealed 3 fatal bugs, 5 significant discrepancies, and 3 deployment errors. All were traced to specific byte-level mismatches between our code and Berny23's socket/API contracts.

---

### 🔴 FATAL FIX #1: `position:1` Hardcoded for All Zones

**File:** `deck/overlay/main.js` (lines 106, 129)
**Root cause:** The overlay author created Left/Center/Right zones but never mapped zone names to Berny23's `pad-num` values (center=1, left=2, right=3). Every `POST /place` sent `position:1` regardless of which zone the user clicked.

**Before (broken):**
```js
// main.js line 106 — every zone reports as Center
await fetch('/place', {method:'POST', headers:{'Content-Type':'application/json'},
  body:JSON.stringify({uid:tb.uid, id:tb.id, position:1, index:target})});
//                                                ^^^^^^^^^ HARDCODED TO 1
```

**After (fixed):**
```js
// Added zone→position mapping (Berny23's pad-num values)
const ZONE_TO_POSITION = { left: 2, center: 1, right: 3 };

// main.js — now uses correct position per zone
await fetch('/place', {method:'POST', headers:{'Content-Type':'application/json'},
  body:JSON.stringify({uid:tb.uid, id:tb.id, position:ZONE_TO_POSITION[zone], index:target})});
//                                                ^^^^^^^^^^^^^^^^^^^^^^^^^^
```

**Impact without fix:** All tags placed on Left or Right registered as Center position. LED color routing broken — game sends LED command for Left pad (position 2), node-ld routes to tags at position 2, no tags found, LED never lights.

---

### 🔴 FATAL FIX #2: Missing `connectionStatus` and `syncToyPad` Emits on Page Load

**File:** `deck/overlay/main.js` (init function)
**Root cause:** The overlay only LISTENED for server events but never EMITTED `connectionStatus` (which triggers `Connection True` on reload) or `syncToyPad` (which reconciles server's in-memory `tp._tokens` with on-disk `toytags.json`).

**Before (broken):**
```js
async function init() {
  // ... fetch JSON data ...
  renderTabs(); applyFilter(); syncToyBox();
  // 🔴 MISSING: socket.emit('connectionStatus') and socket.emit('syncToyPad')
}
```

**After (fixed):**
```js
async function init() {
  // ... fetch JSON data ...
  renderTabs(); applyFilter(); syncToyBox();
  // ✅ Restored 1:1 Berny23 initialization handshake
  socket.emit('connectionStatus');
  socket.emit('syncToyPad');
}
```

**Impact without fix:** On browser reload during active gameplay, `#meta` stuck on "Connecting to PS3..." forever. Server state and client state diverged because `syncToyPad` never triggered `initializeToyTagsJSON()` reconciliation.

---

### 🔴 FATAL FIX #3: LED Socket Handlers — Array Destructured as Separate Args

**File:** `deck/overlay/main.js` (LED handler block)
**Root cause:** Berny23's server emits `io.emit("Color One", [pad, color])` — a **single array argument**. Socket.IO v4 transmits this as one argument to the client. Our overlay destructured it as `(pad, color)` — receiving the entire array as `pad` and `undefined` as `color`. The check `[Array] >= 1` produced `NaN` → always falsy → glow never triggered.

**Before (broken — all 4 handlers had this pattern):**
```js
// pad = [1, "#ff0000"] (entire array!), color = undefined
socket.on('Color One', (pad, color) => {
  const zoneNames = ['','center','left','right'];
  if (pad >= 1 && pad <= 3) applyZoneGlow(zoneNames[pad]);
  //   ^^^ [Array] >= 1 → NaN → FALSE — NEVER executes!
});
```

**After (fixed — matches Berny23's original `function(e) { e[0], e[1] }` pattern):**
```js
// e = [pad, color] — single array argument, indexed per upstream
socket.on('Color One', (e) => {
  const pad = e[0];
  const zoneNames = ['','center','left','right'];
  if (pad >= 1 && pad <= 3) applyZoneGlow(zoneNames[pad]);
});

// Color All: e = [centerColor, leftColor, rightColor]
socket.on('Color All', (e) => {
  if (e[0]) applyZoneGlow('center');
  if (e[1]) applyZoneGlow('left');
  if (e[2]) applyZoneGlow('right');
});

// Fade One: e = [pad, speed, cycles, color]
socket.on('Fade One', (e) => {
  const pad = e[0];
  const zoneNames = ['','center','left','right'];
  if (pad >= 1 && pad <= 3) applyZoneGlow(zoneNames[pad]);
});

// Fade All: e = [topSpeed, topCycles, topColor, leftSpeed, leftCycles, leftColor, rightSpeed, rightCycles, rightColor]
// Per Berny23 original: center=e[2], left=e[5], right=e[8]
socket.on('Fade All', (e) => {
  if (e[2]) applyZoneGlow('center');
  if (e[5]) applyZoneGlow('left');
  if (e[8]) applyZoneGlow('right');
});
```

**Impact without fix:** Zone glow (keystone puzzle visual feedback) completely dead on all four LED events.

---

### 🟡 SIGNIFICANT FIX #4: `run-ui.sh` Injected sync-api into Wrong File

**File:** `deck/run-ui.sh` (lines 63-67)
**Root cause:** The injection targeted `server/index.js` (which doesn't exist in Berny23's repo — it's `server/index.html`). The grep created a new orphan file that Node never executes. The root `index.js` (where `const app = express()` is defined) was never modified.

**Before (broken):**
```bash
# WRONG: server/index.js doesn't exist in upstream Berny23
if ! grep -q "sync-api" server/index.js 2>/dev/null; then
  echo "try { require('./sync-api')(app); }" >> server/index.js
fi
```

**After (fixed):**
```bash
# CORRECT: target root index.js where app = express() is defined
if ! grep -q "sync-api" index.js 2>/dev/null; then
  echo "try { require('./server/sync-api')(app); }" >> index.js
fi
```

**Impact without fix:** `POST /api/sync-images` returned 404. "Sync Missing Images" button silently failed.

---

### 🟡 SIGNIFICANT FIX #5: `sync-images.js` Doubly-Nested `server/` Paths

**File:** `deck/overlay/sync-images.js` (lines 9-11)
**Root cause:** When deployed by run-ui.sh, the script lives at `server/sync-images.js` so `__dirname` already includes `server/`. The original paths added another `server/` → `server/server/images/` — nonexistent directories.

**Before (broken):**
```js
const IMAGES_DIR = path.join(__dirname, 'server', 'images');  // → server/server/images/  ✗
const CHAR_MAP = path.join(__dirname, 'server', 'json', 'charactermap.json');
const TOKEN_MAP = path.join(__dirname, 'server', 'json', 'tokenmap.json');
```

**After (fixed — auto-detects deployment context):**
```js
// Detects: deployed (server/sync-images.js) vs standalone (deck/overlay/sync-images.js)
const IS_DEPLOYED = fs.existsSync(path.join(__dirname, 'json'));
const BASE = IS_DEPLOYED ? __dirname : path.join(__dirname, 'server');
const IMAGES_DIR = path.join(BASE, 'images');
const CHAR_MAP = path.join(BASE, 'json', 'charactermap.json');
const TOKEN_MAP = path.join(BASE, 'json', 'tokenmap.json');
```

**Impact without fix:** `ENOENT` when spawned child process tried to read character/token maps.

---

### 🟡 SIGNIFICANT FIX #6: `moveFromPad` Delay — 200ms → 500ms

**File:** `deck/overlay/main.js`
**Root cause:** The overlay used 200ms delay after `DELETE /remove` before `POST /place`. Berny23's upstream uses 500ms. The shorter delay risked a race condition where `remove` hadn't flushed before `place`.

```js
// Before: await sleep(200);
// After:  await sleep(500);  // matches upstream's 500ms setTimeout
```

---

### Layout/UX Changes

| Change | Rationale | File |
|--------|-----------|------|
| Sticky ToyPad | Restored `position:sticky; top:0; z-index:30; backdrop-filter:blur(6px)` — matches original `ld-toypad-server` behavior. Keeps pad visible while scrolling catalog. | `main.css` |
| Keystone glow | Restored `@keyframes keystone-glow` + `.zone-lit` / `.zone-lit-sustain` CSS classes. Gold pulsing border during keystone puzzles. 1:1 match with original. | `main.css` |
| Card size doubled | 40px→80px thumbnails, `minmax(80px)`→`minmax(120px)` grid. Characters recognizable at thumbnail size. | `main.css` |
| Collapsible Toy Box | Click `#toyboxToggle` header to slide open/close. Saves vertical space on Deck's 800p screen. | `index.html`, `main.css`, `main.js` |
| Portal telemetry | Restored `#portalTelemetry` element showing which zones the game is lighting up. | `index.html`, `main.js` |
| Status feedback | Restored `#statusLine` with `.ok`/`.error` coloring. Shows "Placed Batman on CENTER" / "Place failed" messages. | `index.html`, `main.css`, `main.js` |
| Catalog count | Restored `#toyCount` showing filtered entry count. | `index.html`, `main.js` |
| Image fallback initials | Restored `.toy-card-fallback` and `.pad-slot-fallback` — shows "BA" when Batman image fails to load. | `main.css`, `main.js` |
| Image sync on-demand | "⬇ Sync Missing Images" button triggers `POST /api/sync-images` with streaming progress bar. Moved from startup to UI-triggered. | `index.html`, `main.js`, `sync-api.js`, `run-ui.sh` |

---

## Part 2: Cross-Analyzer Agent

**File:** `.github/agents/cross-analyzer.agent.md`

### Rationale

After months of separate development, we needed an automated way to catch subtle mismatches between the proven-working Steam Deck codebase (`deck/`) and the abandoned SPRX codebase (`sprx-plugin/`, `ld-toypad-server/`), as well as against Berny23's upstream canonical implementation.

### Capabilities

- Compares 3 codebases: working (`deck/`), upstream (Berny23 GitHub), non-functional (`sprx-plugin/` + `ld-toypad-server/`)
- Checks 6 dimensions: protocol constants, init order/timing, data flow, error handling, state management, communication model
- Produces discrepancy tables with exact values, severity flags (🔴 FATAL / 🟡 SIGNIFICANT / ⚪ MINOR), execution traces, file+line citations
- Read-only tools: `read`, `search`, `agent`, `web`
- User-invocable from chat picker or as subagent

### Example: What It Caught

The agent identified that the overlay's `position:1` hardcode would cause all tags to register as Center position by comparing `deck/overlay/main.js:106` against the upstream's pad-num attribute system. It also caught the Socket.IO array destructuring mismatch by verifying Berny23's `io.emit("Color One", [pad, color])` single-argument pattern against our multi-argument destructuring.

---

## Part 3: Composite USB Gadget Analysis — Expert Round 1

The expert proposed a composite USB gadget (ToyPad + DS3 on one cable). Our analysis identified 3 fatal blockers confirmed by the expert:

| # | Blocker | Root Cause | Expert Confirmation |
|---|---------|-----------|---------------------|
| 1 | VID/PID clashing | Composite devices share a single Device Descriptor. Can't be `0x0E6F:0x0241` AND `0x054C:0x0268`. No true hub emulation in configfs. | ✅ Confirmed — "cannot have two different Device-Level VID/PIDs on a single USB cable without true Hub Emulation" |
| 2 | DS3 ep0 authentication | `usb_f_hid` (hidg) can't intercept Feature Reports on Endpoint 0. PS3 sends `GET_REPORT` (0xF2, 0xF5) and `SET_REPORT` (0xEF). Without correct responses, controller never gets a player slot. | ✅ Confirmed — "The vanilla Linux usb_f_hid does not support intercepting or responding to Feature Reports on Endpoint 0" |
| 3 | 80/8 HID descriptor | Expert's proposed ToyPad descriptor (80-byte INPUT, 8-byte OUTPUT) was the old incorrect assumption. Our verified 32/32 descriptor is correct. | ✅ Confirmed — "Your 29-byte descriptor is the absolute truth" |

### Expert's Recommendation

"ABANDON THE COMPOSITE GADGET APPROACH. Leave the Steam Deck configured solely as the LEGO Dimensions ToyPad. Play using a standard DualShock 3/4/DSense connected natively to the PS3 via Bluetooth."

---

## Part 4: DS3 One-Time Pairing via FunctionFS — Expert Round 2

### Architecture

Since simultaneous composite is impossible, we split across buses:

```
PHASE 1 (One-time — 30 seconds):         PHASE 2 (Every session):
┌──────────┐    USB (FFS gadget)         ┌──────────┐    Bluetooth (DS3)
│  Deck    │ ──────────────────→ PS3     │  Deck    │ ← ─ ─ ─ ─ →  PS3
│  FFS     │  ep0 auth ✓                │  BT MAC  │              Player 1
│  daemon  │  GET_REPORT 0xF2,0xF5 ✓    │  spoofed │   USB (configfs)
│          │  SET_REPORT 0xF5 ✓         │          │ ────────────→  PS3
│          │  PS3 MAC captured ✓        │  ToyPad  │   ToyPad
│          │  → pairing.json            │  gadget  │   (verified)
└──────────┘                            └──────────┘
```

The FFS daemon runs ONCE to complete the Bluetooth pairing handshake. After that, the Deck connects as a wireless DS3 while USB remains the proven ToyPad. No VID/PID conflict — they're on different buses.

### Key Insight: FFS is in SteamOS Kernel

User confirmed: `CONFIG_USB_F_FS` is enabled in SteamOS. This removes the expert's original third blocker ("SteamOS doesn't ship the needed kernel modules").

---

## Part 5: New Files Created

### `deck/ds3-pair-daemon.c`

**Purpose:** Minimal FFS-based DS3 emulator for one-time Bluetooth pairing.
**Source:** Ported from RosettaPad (`github.com/ihasTaco/RosettaPad`) Android JNI to plain Linux C.
**Size:** ~500 lines

**Key components:**

1. **FFS descriptor setup** — Interface class `0x03` (HID), EP1 IN (interrupt, 64-byte, bInterval=1), EP2 OUT (interrupt, 64-byte, bInterval=1). No HID report descriptor — RosettaPad declares HID class via interface descriptor and handles protocol manually via ep0.

```c
static const struct {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count; __le32 hs_count;
    struct { struct usb_interface_descriptor intf;
             struct usb_endpoint_descriptor_no_audio ep_in, ep_out; }
        __attribute__((packed)) fs_descs, hs_descs;
} __attribute__((packed)) ffs_descriptors = {
    .header = { .magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2, .length = sizeof(ffs_descriptors),
                .flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC },
    .fs_count = 3, .hs_count = 3,
    .fs_descs = {
        .intf = { .bLength=9, .bDescriptorType=USB_DT_INTERFACE, .bInterfaceNumber=0,
                  .bNumEndpoints=2, .bInterfaceClass=0x03, .iInterface=1 },
        .ep_in = { .bEndpointAddress=0x81, .bmAttributes=0x03, .wMaxPacketSize=64, .bInterval=1 },
        .ep_out = { .bEndpointAddress=0x02, .bmAttributes=0x03, .wMaxPacketSize=64, .bInterval=1 },
    },
};
```

2. **ep0 control transfer handler** — Responds to all 7 PS3 feature report requests:

```c
switch (report_id) {
    case DS3_REPORT_CAPS:    data = report_01; break;  // 0x01 — Capabilities (64 bytes)
    case DS3_REPORT_MAC:     data = report_f2; break;  // 0xF2 — Controller BT MAC (bytes 4-9)
    case DS3_REPORT_PAIRING: data = report_f5; break;  // 0xF5 — Host BT MAC (bytes 2-7)
    case DS3_REPORT_CALIB:   data = report_f7; break;  // 0xF7 — Calibration
    case DS3_REPORT_STATUS:  data = report_f8; break;  // 0xF8 — Status
    case DS3_REPORT_EF:      data = report_ef; break;  // 0xEF — Configuration echo
}
```

3. **PS3 MAC capture** — When PS3 sends `SET_REPORT 0xF5`, bytes 2-7 contain the PS3's Bluetooth MAC:

```c
if (report_id == DS3_REPORT_PAIRING && r >= 8) {
    memcpy(g_ps3_mac, &buf[2], 6);  // Capture PS3's BT address
    g_ps3_mac_valid = 1;
    memcpy(&report_f5[2], &buf[2], 6);  // Update for future GET_REPORT
    g_handshake_complete = 1;
}
```

4. **Pairing data persistence** — Writes JSON with both MACs:

```json
{
  "deck_bt_mac": "XX:XX:XX:XX:XX:XX",
  "ps3_bt_mac": "YY:YY:YY:YY:YY:YY"
}
```

5. **Idle input report stream** — Keeps EP1 IN alive with 49-byte idle reports at ~125Hz:

```c
static uint8_t input_report[49] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x80, 0x80,   // sticks centered
    // ... (buttons all zero, accel/gyro at rest) ...
    0x02  // final byte
};
```

**Build command:**
```bash
gcc -Wall -O2 -o ds3-pair-daemon ds3-pair-daemon.c
```

---

### `deck/bt-connect-ds3.sh`

**Purpose:** Bluetooth reconnection script. Reads `ds3-pairing.json`, spoofs Deck's BT MAC, connects to PS3.

**Flow:**
```bash
# 1. Read MACs from pairing file
DECK_MAC=$(grep -oP '"deck_bt_mac"' ...)
PS3_MAC=$(grep -oP '"ps3_bt_mac"' ...)

# 2. Spoof BT MAC to match paired identity
sudo hciconfig hci0 down
sudo hciconfig hci0 addr "$DECK_MAC"
sudo hciconfig hci0 up

# 3. Load hid-sony kernel module
sudo modprobe hid-sony

# 4. Connect to PS3
bluetoothctl connect "$PS3_MAC"
```

**Restore original MAC on failure:**
```bash
sudo hciconfig hci0 down
sudo hciconfig hci0 addr "$ORIG_MAC"
sudo hciconfig hci0 up
```

---

### `deck/run-ui-sync-ds3.sh`

**Purpose:** Unified launcher — clone of `run-ui.sh` with `--pair-controller` mode.

**Modes:**

| Flag | Effect |
|------|--------|
| (none) | Identical to `run-ui.sh` — ToyPad only |
| `--pair-controller` | Pair DS3 via FFS, then build ToyPad, launch emulator |
| `--pair-controller-only` | Only run FFS pairing daemon, exit |

**One-time setup:**
```bash
sudo ./run-ui-sync-ds3.sh --pair-controller
```

**Every session thereafter:**
```bash
sudo ./bt-connect-ds3.sh        # Deck → wireless DS3
sudo ./run-ui-sync-ds3.sh        # Deck → wired ToyPad
```

**Key design decision:** The FFS daemon and ToyPad gadget use different ConfigFS directories (`ds3-pair` vs `g1`) and different UDC bindings. The script properly tears down the DS3 gadget before building the ToyPad. `run-ui.sh` is preserved unchanged.

---

### `deck/overlay/sync-api.js`

**Purpose:** Express route handler for `POST /api/sync-images`. Spawns `sync-images.js` as child process, streams NDJSON progress to client.

**Injection mechanism (run-ui.sh):**
```bash
# Appended to Berny23's root index.js (one-time, idempotent):
try { require('./server/sync-api')(app); } catch(e) { ... }
```

---

## Part 6: Targeted Expert Questions (Round 3)

### 🔴 Question 1: Bluetooth MAC Spoofing on Steam Deck Hardware

The `ds3-pair-daemon.c` reports the Deck's Bluetooth MAC (from `hciconfig hci0`) in `GET_REPORT 0xF2` bytes 4-9 and `GET_REPORT 0xF5` bytes 2-7. `bt-connect-ds3.sh` then spoofs this MAC via `hciconfig hci0 addr`.

**Question:** Does the Steam Deck's Bluetooth chipset (Qualcomm/Atheros or Realtek, depending on LCD vs OLED model) support MAC address changes via `hciconfig`? Some Broadcom chipsets reject MAC changes with `Not supported (95)`. If `hciconfig` fails, does `btmgmt public-addr` work on SteamOS's BlueZ version?

**Test command to run on Deck:**
```bash
ORIG=$(hciconfig hci0 | grep 'BD Address' | awk '{print $3}')
sudo hciconfig hci0 down
sudo hciconfig hci0 addr 00:11:22:33:44:55
sudo hciconfig hci0 up
NEW=$(hciconfig hci0 | grep 'BD Address' | awk '{print $3}')
echo "Original: $ORIG → New: $NEW"
# Restore:
sudo hciconfig hci0 down && sudo hciconfig hci0 addr "$ORIG" && sudo hciconfig hci0 up
```

---

### 🔴 Question 2: PS3 Bluetooth Connection Direction

RosettaPad's `bt_hid.c` scans for the PS3 by Sony OUI prefixes and **initiates** the Bluetooth L2CAP connection on PSM 0x0011 (control) and 0x0013 (interrupt). This is the opposite direction of a normal DS3 pairing — normally the PS3 initiates.

**Question:** Does the PS3 accept an **inbound** Bluetooth connection from a previously-USB-paired controller? Or must the PS3 always be the initiator? If the PS3 must initiate, is there a way to trigger a reconnection attempt (e.g., by briefly binding/unbinding the USB gadget, or by pressing the PS button on a real controller)?

**RosettaPad's approach (Android/Java):**
```java
// bt_hid.c equivalent — scans for PS3 by OUI, connects L2CAP
String ps3OUI = "00:1E:3C";  // or 00:26:43, 00:1C:BE, etc.
BluetoothDevice ps3 = findDeviceByOUI(ps3OUI);
ps3.createL2capChannel(0x0011);  // Control PSM
ps3.createL2capChannel(0x0013);  // Interrupt PSM
```

---

### 🔴 Question 3: Feature Report 0xF5 Dual Role (Host MAC vs PS3 MAC)

The `report_f5` template serves dual purpose: before pairing, bytes 2-7 contain the Deck's BT MAC (so the PS3 learns what to pair with). After `SET_REPORT 0xF5`, the daemon overwrites bytes 2-7 with the PS3's MAC (so future `GET_REPORT` calls return the paired address).

**Question:** Is this the correct behavior? Does the PS3 expect `GET_REPORT 0xF5` to return the PS3's own MAC after pairing (indicating "I am paired to this PS3"), or should it always return the controller's MAC? RosettaPad overwrites the field — confirming this is correct behavior for their Android-to-PS3 setup. Does the PS3 behave the same way when pairing with a real DualShock 3?

**Code in question:**
```c
// During pairing: report_f5[2..7] = Deck's BT MAC
memcpy(&report_f5[2], deck_mac, 6);

// After SET_REPORT 0xF5: report_f5[2..7] = PS3's BT MAC
memcpy(&report_f5[2], &buf[2], 6);  // PS3 wrote its own MAC
```

---

### 🟡 Question 4: `bInterval` Value for DS3 Endpoint Descriptors

The FFS daemon uses `bInterval = 1` for both EP1 IN and EP2 OUT (interrupt endpoints). For full-speed USB, `bInterval=1` means 1ms polling. For high-speed USB, `bInterval=1` means 125µs (2^(1-1) × 125µs).

**Question:** What `bInterval` does a real DualShock 3 advertise? The DS3 is a full-speed USB 1.1 device (`bcdUSB = 0x0110`), so `bInterval=1` = 1ms (1000Hz). Is this correct, or should it be higher (e.g., `bInterval=4` = 4ms / 250Hz, or `bInterval=8` = 8ms / 125Hz)?

**Current FFS descriptor:**
```c
.ep_in = {
    .bLength = 7, .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = 0x81,
    .bmAttributes = 0x03,        // Interrupt
    .wMaxPacketSize = 64,
    .bInterval = 1,              // 1ms (FS) or 125µs (HS)
},
```

---

### 🟡 Question 5: Feature Report 0x01 Byte-Per-Byte Verification

The `report_01` (capabilities) template was taken directly from RosettaPad. However, RosettaPad may have customized these bytes for their specific DualSense→DS3 translation.

**Question:** Are these 64 bytes the canonical values for a genuine DualShock 3 (CECHZC2U), or are they RosettaPad-specific? If they differ from a real DS3, would the PS3 reject the device during the `GET_REPORT 0x01` phase?

**Current template (byte 0-7 shown):**
```c
static uint8_t report_01[64] = {
    0x00, 0x01, 0x04, 0x00, 0x08, 0x0C, 0x01, 0x02,
    // ... 56 more bytes ...
};
```

---

### 🟡 Question 6: PS3 Controller Slot Assignment After USB Unplug

During the one-time pairing, the PS3 assigns a controller slot (Player 1 LED) to the USB gadget. When the daemon tears down the FFS gadget (unbinds UDC), the PS3 sees a USB disconnect.

**Question:** Does the PS3 immediately release the controller slot, allowing a Bluetooth controller to claim it? Or does it hold the slot in a "disconnected" state? If the slot is held, does rebooting the PS3 (with the Deck connecting via Bluetooth on next boot) resolve the slot assignment?

**Observed sequence in RosettaPad:**
1. USB paired → PS3 assigns Player 1
2. USB unplugged → RosettaPad immediately initiates Bluetooth connection
3. PS3 recognizes the paired BT MAC → reassigns same Player 1 slot over Bluetooth

**Question for our setup:** Since our daemon exits after pairing (we don't immediately switch to Bluetooth), will the PS3 still remember the paired BT MAC across a reboot? i.e., can we pair today via USB, reboot the PS3 tomorrow, run `bt-connect-ds3.sh`, and have it work?

---

### 🟡 Question 7: USB Gadget Mode + Bluetooth Coexistence on Steam Deck

The final architecture has the Deck's USB-C port in DRD/gadget mode (ToyPad) while Bluetooth is simultaneously active (wireless DS3 controller).

**Question:** Is there any known hardware conflict between USB gadget mode and Bluetooth on the Steam Deck's specific SoC (Valve Jupiter / AMD Aerith)? Specifically:
- Do they share an internal USB bus that could cause bandwidth contention?
- Does the USB-C cable's EMI affect Bluetooth signal quality at the ~1-2m distance typical between a Deck on a stand and a PS3?
- Has anyone tested simultaneous configfs USB gadget + Bluetooth HID on SteamOS?

---

### ⚪ Question 8: `ds3-pair-daemon.c` Compilation Dependencies

The daemon uses only Linux kernel headers (`linux/usb/functionfs.h`) and standard C libraries. No external dependencies.

**Question:** Is `linux/usb/functionfs.h` available in SteamOS's default `linux-headers` package? Or does it require installing `linux-headers` separately via pacman? (The script handles this with `pacman -S gcc`, but headers may need an additional package.)

---

## Summary: Files Changed This Session

| File | Type | Description |
|------|------|-------------|
| `.github/agents/cross-analyzer.agent.md` | New | Custom agent for codebase cross-analysis |
| `deck/overlay/index.html` | Modified | Sticky pad, telemetry, status, count, sync button, collapsible toybox, sync modal |
| `deck/overlay/main.css` | Modified | Sticky, keystone glow, collapsed state, 80px cards, status line, sync button |
| `deck/overlay/main.js` | Modified | Position fix, socket emits, LED handlers, toggle, fallbacks, sync trigger, status feedback |
| `deck/overlay/sync-api.js` | New | Express route handler for on-demand image sync |
| `deck/overlay/sync-images.js` | Modified | Auto-detect deployment context for paths |
| `deck/run-ui.sh` | Modified | Corrected sync-api injection target, downloads sync scripts |
| `deck/ds3-pair-daemon.c` | New | FFS-based DS3 Bluetooth pairing daemon (~500 lines C) |
| `deck/bt-connect-ds3.sh` | New | Bluetooth reconnection script |
| `deck/run-ui-sync-ds3.sh` | New | Unified launcher (ToyPad + optional DS3 pairing) |
| `docs/expert-questions-composite-gadget-2026-08-04.md` | New | Round 1 expert questions (composite gadget analysis) |
| `docs/expert-questions-gadget-cycling-2026-08-04.md` | New | Round 2 expert questions (gadget cycling + Bluetooth) |
| `docs/expert-questions-ds3-pairing-2026-08-05.md` | New | Round 2b expert questions (one-time FFS pairing) |
| `docs/CHANGELOG-2026-08-05-overlay-ds3-pairing.md` | New | This document |

**Preserved unchanged:** `deck/run.sh`, `deck/deck_toypad.sh`, all `sprx-plugin/`, all `ld-toypad-server/`
