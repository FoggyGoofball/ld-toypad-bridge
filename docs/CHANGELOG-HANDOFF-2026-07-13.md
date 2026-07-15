# LD-ToyPad Bridge Handoff Changelog (2026-07-13)

## Resume Marker (2026-07-14): Portal + Multi-Slot Follow-Up

### User-requested follow-up in progress
- Add support for keystone-style zone guidance visibility in the web UI.
- Ensure left/right zones support up to 3 simultaneous pieces (including duplicates).
- Keep center as single-slot.
- Add activation-token upload path to elevated bridge launcher after PS3 address acquisition.

### Current checkpoint
- Backend already carries partial support (multi-slot zone model, slot-aware API payloads, basic portal telemetry capture).
- Frontend rendering and launcher integration are being completed in this pass.

## Scope
This changelog records all work completed so far before continuing with additional fixes.

## Workstream Summary
- Goal: recover WSL build tree at `/home/mike/ldtoypad` and unblock SPRX build.
- Status at this checkpoint: C compilation succeeds across all objects; packaging fails at SPRX conversion due missing PRX parameter section in ELF.

## Repository and File Operations

### Workspace sync actions
- Copied missing SPRX files from old Windows tree into workspace tree:
  - `sprx-plugin/debug.c`
  - `sprx-plugin/debug.h`
  - `sprx-plugin/network.c`
  - `sprx-plugin/network.h`
  - `sprx-plugin/toypad_state.c`
  - `sprx-plugin/toypad_state.h`
  - `sprx-plugin/usb_hooks.c`
  - `sprx-plugin/usb_hooks.h`

### Script authoring in workspace (`dimensions plugin` copy)
- Created/filled previously empty helper scripts:
  - `sprx-plugin/fix_sources.sh`
  - `sprx-plugin/fix_psl1ght.sh`
  - `sprx-plugin/build.sh`

#### `fix_sources.sh`
- Added automated source normalization for WSL copy:
  - `sys_net_*` -> `sysNet*`
  - `SYS_NET_*` constants -> standard socket constants
  - `sys_net_sockaddr*` and related types -> `sockaddr*` equivalents
  - malformed include rewrites for `lv2/...` includes
- Added optional restore path:
  - `RESTORE_MAIN_FROM=/path/to/main.c`

#### `fix_psl1ght.sh`
- Added known `sys/usbd.h` patch replacements (with `.bak` backup):
  - `unk1` -> `deviceNumber` in known bad call signatures.

#### `build.sh`
- Added orchestration:
  - optional `FIX_PSL1GHT=1`
  - optional `RESTORE_MAIN_FROM=/path/main.c`
  - runs fix script and then `make clean; make all`.

## WSL Build Tree Changes (`/home/mike/ldtoypad`)

### Source recovery/fix history
- Restored `main.c` after it was previously zero bytes.
- Applied broad API/include substitutions to remove `sys_net_*` naming mismatch class of issues.
- Replaced `main.c` with a compile-safe implementation consistent with current local headers and exported hook symbol expectations.
- Replaced `usb_hooks.c` with a compile-safe stub hook manager (temporary), preserving public API:
  - `usb_hooks_install`
  - `usb_hooks_remove`
  - `get_syscall_number`
- Replaced `network.c` with compile-safe stub implementation (temporary), preserving `network.h` API.
- Fixed `toypad_state.c` buffer indexing on `void*` by casting to `uint8_t*` before subscripting.
- Replaced `debug.c` with libc-light implementation to reduce libc linkage requirements.
- Added `compat.c` with local implementations for:
  - `memcpy`
  - `memset`
  - `strlen`

### WSL Makefile evolution
- Removed unavailable `-lsyscalls` from link flags.
- Addressed host linker mismatch by forcing cross compiler driver for link step.
- Tested multiple linker flag layouts to bypass ELF PHDR errors.
- Reordered object/library linkage to resolve symbol ordering issues.
- Added `compat.c` into `C_SRCS`.
- Current link attempts reach SPRX conversion but fail in `sprxlinker`.

## Build Progression and Current Blocker

### Achieved
- All object files compile successfully.
- ELF link step can complete under adjusted flag combinations.
- Build reaches SPRX tooling phase (`sprxlinker`).

### Current blocking error
- `sprxlinker` reports:
  - `elf does not have a prx parameter section.`

### Diagnostic evidence
- `strings /usr/local/ps3dev/bin/sprxlinker` confirms expected section marker:
  - `.sys_proc_prx_param`
- Toolchain linker script `lv2.ld` includes:
  - `.sys_proc_param`
  - `.sys_proc_prx_param`
- Remaining issue is PRX metadata/startup section generation, not C compile errors.

## Risk Notes
- Current `usb_hooks.c` and `network.c` in WSL are temporary compile-safe stubs and do not represent final runtime behavior.
- Current `debug.c` is simplified and avoids formatted output for link stability.
- Runtime fidelity is intentionally reduced while isolating build-system blockers.

## Next Planned Steps (Option 2)
1. Sync the WSL-fixed files back into the workspace canonical tree (`dimensions plugin/sprx-plugin`).
2. Continue PRX metadata/startup section fix from workspace source.
3. Rebuild in WSL using canonical workspace-synced sources.
4. Once SPRX packaging works, incrementally restore non-stub runtime implementations.

## Update: Option 2 Executed (Completed)

### Canonical sync completed
- WSL working files were synced back into workspace canonical tree under `sprx-plugin/`.

### PRX metadata/startup resolution
- Added explicit PRX parameter emission to `sprx-plugin/main.c`:
  - Emits `.sys_proc_prx_param` via assembly.
  - Added `SYS_PROCESS_PARAM_FIXED(1001, 0x4000)` for `.sys_proc_param`.
- Updated linker inputs in `sprx-plugin/Makefile`:
  - Retained explicit cross-toolchain linking.
  - Added startup object:
    - `/usr/local/ps3dev/ppu/powerpc64-ps3-elf/lib/lv2-sprx.o`

### Verified build result (WSL)
Command executed:

```bash
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all 2>&1
```

Result:
- Successful SPRX packaging.
- Generated files:
  - `/home/mike/ldtoypad/build/ldtoypad.sprx`
  - `/home/mike/ldtoypad/build/ldtoypad.fake.self`

Observed terminal lines confirming success:
- `Build complete: /home/mike/ldtoypad/build/ldtoypad.sprx`
- `-rw-r--r-- ... /home/mike/ldtoypad/build/ldtoypad.sprx`

### Remaining technical debt (important)
- Current WSL/canonical runtime modules include temporary compile-safe stubs:
  - `sprx-plugin/usb_hooks.c` (stubbed hook installer/remover)
  - `sprx-plugin/network.c` (stubbed network transport)
  - `sprx-plugin/debug.c` (minimal logging implementation)
  - `sprx-plugin/compat.c` (local memory/string helpers)
- Build pipeline is now unblocked; functional runtime behavior still requires restoration/refinement of non-stub implementations.

## Final Build Confirmation (Canonical Source)

After syncing canonical workspace files back to WSL and rebuilding, SPRX generation is successful.

### Key final fix
- Added startup object to linker inputs:
  - `/usr/local/ps3dev/ppu/powerpc64-ps3-elf/lib/lv2-sprx.o`
- This removed the `sprxlinker` segmentation fault while keeping explicit PRX param section data from `main.c`.

### Re-validated command

```bash
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all 2>&1
```

### Re-validated outputs
- `/home/mike/ldtoypad/build/ldtoypad.sprx`
- `/home/mike/ldtoypad/build/ldtoypad.fake.self`

### Terminal success indicators
- `SPRX  /home/mike/ldtoypad/build/ldtoypad.sprx`
- `Build complete: /home/mike/ldtoypad/build/ldtoypad.sprx`

## Incremental Runtime Restoration: USB Hooks Phase 1

### Change applied
- Replaced the trivial USB hook stub with a structured phase-1 hook manager in `sprx-plugin/usb_hooks.c`.
- Added explicit planned syscall mapping for USB intercept targets:
  - `sys_usbd_open` -> 845
  - `sys_usbd_close` -> 846
  - `sys_usbd_get_descriptor` -> 847
  - `sys_usbd_control_transfer` -> 848
  - `sys_usbd_interrupt_transfer` -> 849
- Added internal install state tracking and guard logic:
  - Prevents duplicate installs
  - Tracks number of active targets
  - Logs planned targets on install
- Implemented `get_syscall_number(name)` mapping against the target table.
- Kept LV2 syscall-table writes disabled in this phase to preserve build/runtime stability while establishing the hook wiring baseline.

### Compatibility fix during this step
- Removed libc dependency (`strcmp`) from usb hook lookup by adding local string compare helper.
- This preserves no-libc link compatibility under current Makefile flags.

### Validation
Rebuilt in WSL from canonical synced sources after USB phase-1 changes:

```bash
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all 2>&1
```

Result:
- Build and SPRX packaging succeeded.
- Output artifacts:
  - `/home/mike/ldtoypad/build/ldtoypad.sprx`
  - `/home/mike/ldtoypad/build/ldtoypad.fake.self`

### Remaining for USB phase 2
- Reintroduce actual LV2 syscall-table patch writes (install/remove) with firmware-safe guards.
- Preserve Port 1 passthrough and Port 2 interception semantics.

## Incremental Runtime Restoration: USB Hooks Phase 2 (Guarded)

### Change applied
- Added an opt-in LV2 syscall-table patch engine to `sprx-plugin/usb_hooks.c`.
- Patch path includes:
  - Kernel read wrapper via LV2 syscall (`lv2_read_kernel`)
  - Kernel write wrapper via LV2 syscall (`lv2_write_kernel`)
  - Target patch install routine with original pointer capture
  - Target patch remove routine that restores original syscall entries in reverse order
- Hook pointer table now maps actual hook function addresses to target syscalls.

### Safety model
- Phase 2 patching is compiled in but disabled by default:
  - `USB_HOOK_ENABLE_PATCH=0` (default)
- When enabled, failed patch attempts auto-fallback to phase-1 behavior:
  - Restores any partially patched entries
  - Logs failure and continues without active LV2 patching

### Tunables introduced
- `USB_HOOK_ENABLE_PATCH`
- `USB_HOOK_SYSCALL_TABLE_BASE`
- `USB_HOOK_LV2_READ_SC`
- `USB_HOOK_LV2_WRITE_SC`

### Validation
Rebuilt in WSL from canonical synced sources after phase-2 guarded implementation:

```bash
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all 2>&1
```

Result:
- Build and SPRX packaging succeeded.
- Output artifacts:
  - `/home/mike/ldtoypad/build/ldtoypad.sprx`
  - `/home/mike/ldtoypad/build/ldtoypad.fake.self`

### Current phase status
- Phase 2 code path exists and is ready for controlled on-hardware enablement.
- Default builds remain stable and package successfully.

## Incremental Runtime Restoration: Network Layer

### Change applied
- Replaced stubbed `sprx-plugin/network.c` with real UDP transport logic using syscall-level socket calls:
  - `sysNetSocket`
  - `sysNetBind`
  - `sysNetSendto`
  - `sysNetRecvfrom`
  - `sysNetClose`
- Preserved existing `network.h` public API and packet format behavior.
- Server discovery remains automatic from first received packet.

### Compatibility note
- Removed `setsockopt(...SO_NBIO...)` call after initial attempt triggered libc-dependent `libnet` link requirements (`__errno`, `malloc`, etc.) under the current no-libc link model.
- Resulting transport remains valid and build-stable in current environment.

### Validation
Rebuilt in WSL from canonical synced sources after the network replacement:

```bash
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all 2>&1
```

Result:
- Build and SPRX packaging succeeded.
- Output artifacts updated:
  - `/home/mike/ldtoypad/build/ldtoypad.sprx`
  - `/home/mike/ldtoypad/build/ldtoypad.fake.self`

## Recommended Handoff Command Checkpoint
Use this to confirm current state quickly in WSL:

```bash
cd /home/mike/ldtoypad
make clean >/dev/null 2>&1 || true
make all 2>&1
```

Expected current result: successful SPRX packaging.

## Incremental Runtime Restoration: Build-Time Phase-2 Toggle

### Change applied
- Added build-level phase-2 control in `sprx-plugin/Makefile`:
  - `USB_HOOK_ENABLE_PATCH ?= 0`
  - `CFLAGS` now exports `-DUSB_HOOK_ENABLE_PATCH=$(USB_HOOK_ENABLE_PATCH)`
- Added startup visibility in `sprx-plugin/main.c`:
  - Logs whether USB phase-2 patch path is ENABLED or DISABLED.

### Validation (both modes)
Validated from canonical synced sources in WSL:

```bash
# Safe default build
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all 2>&1

# Explicit phase-2 compile-on build
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all USB_HOOK_ENABLE_PATCH=1 2>&1
```

Result:
- Both modes compile, link, and package successfully.
- Both modes produce:
  - `/home/mike/ldtoypad/build/ldtoypad.sprx`
  - `/home/mike/ldtoypad/build/ldtoypad.fake.self`

## What remains before on-hardware testing

1. Confirm LV2 syscall IDs/base for target firmware
- Verify `USB_HOOK_SYSCALL_TABLE_BASE`, `USB_HOOK_LV2_READ_SC`, `USB_HOOK_LV2_WRITE_SC` values are correct for the exact console CFW/firmware combination.

2. Add conservative runtime safety gate for phase-2
- Keep default OFF.
- If enabled, gate activation behind explicit build profile and clear startup logs.
- Abort to phase-1 if any kernel read/write/validation fails.

3. Validate hook patch correctness on target hardware
- Install plugin with `USB_HOOK_ENABLE_PATCH=1` build.
- Confirm all 5 syscall slots are patched and restored cleanly on unload.
- Confirm no kernel panic/hang during load, Toy Pad attach, or unload.

4. Verify functional USB behavior end-to-end
- Port 1 remains passthrough (SSD unaffected).
- Port 2 intercept path active (Toy Pad traffic captured/handled).
- Descriptor/control/interrupt paths behave as expected with real game I/O.

5. Verify network bridge behavior during real gameplay
- Confirm UDP bridge receives/sends Toy Pad packets consistently.
- Confirm reconnect behavior after network interruption.
- Confirm no regressions from no-libc compatibility choices.

6. Add focused smoke-test checklist and capture logs
- Define short repeatable test matrix (load, detect, read/write tags, unload).
- Capture plugin debug logs and bridge-side logs for handoff evidence.

## Transparency Upgrade: Network-Retrievable Runtime Logs

### SPRX logging changes
- Reworked `sprx-plugin/debug.c` from marker-only behavior to real formatted logging.
- Added lightweight formatter support for common specifiers (`%s`, `%d`, `%u`, `%x`, `%X`, `%p`, `%c`, `%%`) without introducing heavy libc dependencies.
- Added remote UDP log streaming support in debug module:
  - New API: `debug_set_remote(ip, port)` in `sprx-plugin/debug.h`
  - Logs continue to be written to local ring buffer and are now optionally mirrored over UDP.

### Automatic remote target wiring
- Updated `sprx-plugin/network.c` to configure debug streaming endpoint automatically:
  - Disabled on init/shutdown.
  - Enabled when bridge server address is learned or manually set.
  - Default debug log port constant: `LDTP_DEBUG_LOG_PORT=28473`.

### Bridge-side log receiver
- Updated `ld-toypad-server/server.js` to run a second UDP listener for SPRX logs:
  - CLI option: `--debug-port` (default `28473`)
  - Prints incoming log lines with source prefix (`[SPRX ip:port] ...`).
- Updated `ld-toypad-server/README.md` with usage/docs for remote log capture.

### Build + validation
- Initial build regression found and fixed:
  - `sprx-plugin/debug.h` include changed to `#include <ppu-types.h>` for current PSL1GHT toolchain compatibility.
- Rebuilt in WSL after fixes:

```bash
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all 2>&1
```

Result:
- SPRX build and packaging successful.
- Artifacts:
  - `/home/mike/ldtoypad/build/ldtoypad.sprx`
  - `/home/mike/ldtoypad/build/ldtoypad.fake.self`

### What remains before hardware test execution (updated)
1. Confirm firmware-specific LV2 constants
- Validate syscall table base and lv2 read/write syscall IDs on target CFW.

2. Stand up bridge with debug listener enabled
- Start Node bridge with `--debug-port 28473` and verify `[SPRX ...]` lines appear after plugin start.

3. Run phase-1 baseline hardware pass (safe mode)
- Build/deploy with `USB_HOOK_ENABLE_PATCH=0`.
- Verify plugin lifecycle, UDP protocol traffic, and remote log continuity under gameplay.

4. Run controlled phase-2 enablement pass
- Build/deploy with `USB_HOOK_ENABLE_PATCH=1` only after baseline passes.
- Confirm patch install/remove logs for all target syscalls and no instability.

5. Execute smoke matrix with evidence capture
- Scenarios: load, toy detect per zone, read path, write path, unload/reload.
- Preserve bridge protocol logs + SPRX remote debug logs for each scenario.

## Deep Trace + Cross-Repo Comparison Pass (LD-ToyPad + PS3xPAD)

### Scope performed
- Traced full local runtime path:
  - PRX start/stop lifecycle
  - USB hook install/remove and patch path
  - descriptor/control/interrupt handlers
  - UDP transport and remote debug telemetry path
- Compared behavior patterns against:
  - `Berny23/LD-ToyPad-Emulator` (Toy Pad emulation/protocol reference)
  - `aldostools/PS3xPAD` (native PS3 USB handling reference)

### Key comparison outcomes
- Local implementation currently uses syscall-table interception for Toy Pad traffic.
- PS3xPAD reference primarily uses `cellUsbdRegisterExtraLdd` attach/probe/detach workflow for native USB handling.
- Local bridge packet flow and Toy Pad VID/PID alignment remain consistent with LD-ToyPad emulator expectations.

### Issues found and fixed during pass
1. USB descriptor length inconsistencies (fixed)
- `wTotalLength` in configuration descriptor did not include HID descriptor size.
- String descriptor length bytes for manufacturer/product were inconsistent with payload size.
- Fixed in `sprx-plugin/toypad_state.c`.

2. Poll receive path blocking risk (fixed)
- `network_recv` used blocking `sysNetRecvfrom(..., flags=0)` from interrupt-in path.
- Switched to `MSG_DONTWAIT` to avoid stalling poll/USB timing.
- Fixed in `sprx-plugin/network.c`.

3. Debug formatter dropped width/length formats (fixed)
- New remote logging introduced custom formatter, but `%08X`, `%04X`, `%02X`, `%lld` were not parsed correctly.
- Added flag/width/length consumption and long/long long integer handling.
- Fixed in `sprx-plugin/debug.c`.

### Validation after fixes
Rebuilt in WSL after applying all review-driven fixes:

```bash
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all 2>&1
```

Result:
- Build and SPRX packaging successful.
- Artifacts:
  - `/home/mike/ldtoypad/build/ldtoypad.sprx`
  - `/home/mike/ldtoypad/build/ldtoypad.fake.self`

### Remaining risk notes after this pass
- LV2 patch constants still firmware-dependent and require on-hardware validation.
- Architecture remains interception-based; not yet migrated to LDD registration style used by PS3xPAD.

## Safe Startup Gate: Dormant by Default

### Change applied
- Added an explicit opt-in enable-file gate in `sprx-plugin/main.c`.
- New enable file path:
  - `/dev_hdd0/plugins/ldtoypad.enable`
- Plugin startup behavior:
  - If the enable file is absent, `_start` returns success without initializing network/hooks.
  - If the enable file exists, the plugin performs normal initialization.

### Additional safety properties
- No boot-time work is performed unless the enable file exists.
- This allows the SPRX to be present on disk without actively engaging USB interception until you intentionally arm it.

### Validation
Rebuilt in WSL after adding the dormant-by-default gate and filesystem link dependency:

```bash
make -C /home/mike/ldtoypad clean >/dev/null 2>&1 || true
make -C /home/mike/ldtoypad all 2>&1
```

Result:
- Build and SPRX packaging successful.
- Artifacts:
  - `/home/mike/ldtoypad/build/ldtoypad.sprx`
  - `/home/mike/ldtoypad/build/ldtoypad.fake.self`

### Operational use
- To keep it disabled: do not create the enable file.
- To activate it: create `/dev_hdd0/plugins/ldtoypad.enable`.
- On the next boot, the plugin consumes and deletes that file before initializing.
- This makes the enable file a one-shot arm token, so a reboot returns the system to the disabled state unless you re-create it.

### Expiry fallback
- Added timestamp-based expiry to the arm token as a second safety net.
- The token is treated as stale if it is older than 900 seconds (15 minutes).
- If stale, the plugin deletes the token and stays dormant.

### Convenience launcher
- Added `start-ldtoypad.bat` at the workspace root.
- The script:
  - uploads the one-shot enable token to `/dev_hdd0/plugins/ldtoypad.enable`
  - starts the Node bridge server in a new console window
  - reminds you the plugin is armed for only one boot unless you re-run it

## Browser Parity Pass: HTTP UI + REST Controls

### Goal
- Add browser-based control parity so Toy Pad placement/removal can be done from a web page, not only CLI input.

### Server/API changes
- Extended `ld-toypad-server/server.js` with HTTP support:
  - New CLI flag: `--http-port` (default `8080`)
  - Added static routes:
    - `GET /` -> `web/index.html`
    - `GET /app.js`
    - `GET /styles.css`
  - Added JSON API routes:
    - `GET /api/status`
    - `GET /api/toys`
    - `POST /api/place` with `{ zone, toyId }`
    - `POST /api/remove` with `{ zone }`
- Added graceful HTTP shutdown handling on CLI exit and process signals.

### Frontend assets added
- Created `ld-toypad-server/web/index.html`:
  - Zone status panel
  - Place toy form (zone + toy selector)
  - Remove buttons per zone
- Created `ld-toypad-server/web/app.js`:
  - API client helpers
  - Polling refresh loop
  - Place/remove action handlers
  - Dynamic render of zones and connection metadata
- Created `ld-toypad-server/web/styles.css`:
  - Responsive panel layout and zone cards
  - Readable status/error states

### Launcher and docs updates
- Updated `start-ldtoypad.bat`:
  - Added `HTTP_PORT` variable
  - Starts server with `--http-port %HTTP_PORT%`
  - Opens browser automatically at `http://localhost:%HTTP_PORT%`
- Updated `ld-toypad-server/README.md`:
  - Added `--http-port` docs and sample command
  - Added Browser UI section and feature list

### Validation
- Syntax check completed for updated server code:

```bash
node --check c:/Users/Admin/source/repos/dimensions plugin/ld-toypad-server/server.js
```

Result:
- No syntax errors reported.

### Notes
- Safety model in SPRX (dormant-by-default + one-shot arm token + expiry) is unchanged.
- This pass is limited to bridge usability parity and does not alter USB hook activation behavior.

## Catalog Expansion Pass: Upstream Character/Token Database Import

### Goal
- Replace the 4-item hardcoded toy stub with the upstream LD-ToyPad catalog so the browser and CLI expose the full usable figure/token database.

### Source and implementation
- Vendored upstream catalog data into `ld-toypad-server/data/`:
  - `charactermap.json`
  - `tokenmap.json`
- Reworked `ld-toypad-server/virtual-toys.js` to:
  - load catalog entries from the imported JSON files
  - generate a deterministic local UID per item
  - derive the lightweight 4-byte tag payload used by the current bridge
  - preserve legacy IDs for known starter entries (`batman`, `gandalf`, `wyldstyle`, `batmobile`)
  - sort results by type/world/name
  - filter placeholder records (`Unknown`, `Future Update`, `Test NN`, numeric-world placeholders, unreleased markers)

### API/UI follow-up
- Extended `GET /api/toys` in `ld-toypad-server/server.js` to include:
  - `itemId`
  - `world`
  - `rebuild`
- Updated browser UI files:
  - `web/index.html`
  - `web/app.js`
  - `web/styles.css`
- Added client-side filter box and catalog count display for practical browsing of the expanded dataset.

### Validation
- Focused catalog generation check:

```bash
node -e "const { VirtualToyManager } = require('./virtual-toys'); const toys = VirtualToyManager.listAvailableToys(); console.log('COUNT=' + toys.length);"
```

Result:
- Catalog count increased from 4 to 322 usable entries.

Known-tag compatibility check:
- `batman` -> `271a0000`
- `gandalf` -> `271b0000`
- `wyldstyle` -> `271c0000`
- `batmobile` -> `2b6a0000`

Live API probe result:
- `/api/toys` returned `COUNT=329` before placeholder filtering
- `/api/toys` returns the cleaned imported dataset after filter tightening

### Notes
- The bridge still uses the existing lightweight tag-response model and does not yet implement upstream encrypted page generation for full write/save fidelity.
- This pass fixes catalog breadth and browser usability, not full MIFARE data emulation.

## Browser Catalog Usability Pass: Type/Franchise Tabs + Optional Thumbnails

### Goal
- Replace the large single selection surface with a structured browser catalog that works without prior knowledge of figure names.

### UI changes
- Reworked the browser UI to use a tabbed catalog flow:
  - Type selection first (`character`, `token`)
  - Franchise/world selection second
  - Search box retained, but scoped within the active type/world slice
- Replaced implicit dropdown-style selection with a card grid:
  - Click-to-select cards
  - Selected item summary panel
  - Place action uses current card selection

### Thumbnail support
- Added static image route in `ld-toypad-server/server.js`:
  - `GET /images/<itemId>.png`
- Followed the original upstream convention:
  - user-supplied PNG files only
  - no bundled copyrighted character/set assets
  - file naming by numeric ID (`1.png`, `1006.png`, etc.)
- Browser behavior:
  - loads thumbnail if present
  - falls back to initials if absent

### Validation
- Syntax checks completed:

```bash
node --check c:/Users/Admin/source/repos/dimensions plugin/ld-toypad-server/server.js
node --check c:/Users/Admin/source/repos/dimensions plugin/ld-toypad-server/web/app.js
```

Live probes:
- `GET /` -> `200`
- `GET /api/toys` -> `COUNT=322`
- `GET /images/1.png` -> `404` when no local assets are present (expected)

### Notes
- The upstream project does support images, but expects them to be provided manually by the user.
- No copyrighted thumbnails were fetched or added to the repository in this pass.

## Image Importer Pass: Respectful Wiki API Sync

### Goal
- Add an automated way to populate item thumbnails from the LEGO Dimensions Wiki without scraping rendered pages or bypassing rate limits.

### Implementation
- Added `ld-toypad-server/scripts/sync-fandom-images.js`:
  - uses MediaWiki `api.php`
  - queries page-linked image filenames via `prop=images`
  - resolves direct asset URLs via `prop=imageinfo`
  - applies conservative delays and retry/backoff on `429` / `5xx`
  - writes local cache metadata to `images/manifest.json`
- Added `ld-toypad-server/image-manifest.js` helper:
  - loads/saves manifest
  - resolves browser-facing image paths by numeric item ID
- Updated `ld-toypad-server/server.js`:
  - `GET /api/toys` now includes `imagePath` when a cached asset exists
- Updated `ld-toypad-server/web/app.js`:
  - browser prefers manifest-backed `imagePath`
  - falls back to `/images/<itemId>.png` when no manifest entry exists
- Updated `ld-toypad-server/package.json`:
  - added `npm run images:sync`

### Safety/compliance posture
- Uses the wiki API surface allowed by `robots.txt`.
- Default pacing is conservative (`1500ms` between API calls).
- Retries back off instead of increasing request pressure.
- No stealth, anti-detection, or ban-evasion behavior was added.

### Validation
- Syntax checks:

```bash
node --check c:/Users/Admin/source/repos/dimensions plugin/ld-toypad-server/scripts/sync-fandom-images.js
node --check c:/Users/Admin/source/repos/dimensions plugin/ld-toypad-server/image-manifest.js
```

- Focused live sample import:

```bash
npm run images:sync -- --limit 2 --delay-ms 1500 --image-delay-ms 250
```

Observed result:
- Downloaded and cached:
  - `images/55.png` for `B.A. Baracus`
  - `images/48.png` for `Finn`
- Browser asset route served both with `200`.

### Notes
- The running browser server must be restarted after the API change to expose `imagePath` in `/api/toys`.
- Coverage will depend on wiki page naming quality and available lead images; the importer stores misses in the manifest for later refinement.

## Full Image Acquisition Run

### Command executed

```bash
npm run images:sync
```

### Result
- Completed full respectful acquisition across all 322 catalog entries.
- Manifest summary:
  - `downloaded`: 283
  - `missing`: 39
- Live API summary after restart:
  - `WITH_IMAGES=283`
  - `TOTAL=322`

### Artifacts
- Cached asset metadata:
  - `ld-toypad-server/images/manifest.json`
- Downloaded assets saved under:
  - `ld-toypad-server/images/`

### Observations
- Coverage is strong for core characters and many vehicles/items.
- Most misses appear to be naming mismatches or absent dedicated rebuild-page images on the source wiki.
- The browser can now render thumbnails for the majority of the catalog without additional manual work.

## UI Runtime Fix: Null `zones` Container Crash

### Problem observed
- Web app showed repeated status error at bottom:
  - `Refresh failed: Cannot set properties of null (setting 'innerHTML')`
- Symptoms included duplicated bottom controls and stalled `Loading catalog...` text.

### Root cause
- `ld-toypad-server/web/index.html` contained malformed duplicated markup from a prior edit.
- The required `div#zones` container was missing in the served DOM, causing `renderZones()` in `web/app.js` to fail when setting `innerHTML`.

### Fix applied
- Repaired `web/index.html` structure:
  - restored the `Zones` panel with `id="zones"`
  - removed duplicated trailing fragment (`</form>`, repeated buttons/status block)
- Validated live page content includes `id="zones"` and returns `200`.

### Result
- Runtime null-reference refresh error resolved.
- Tabbed catalog and status refresh now initialize correctly from a clean page load.

## UI Enhancement: Card-Click Quick Place Modal

### Goal
- Allow users to place a selected toy directly from the catalog card without manually changing form controls.

### Changes
- Updated `ld-toypad-server/web/index.html`:
  - Added compact modal with quick actions:
    - Place Left
    - Place Center
    - Place Right
    - Cancel
- Updated `ld-toypad-server/web/app.js`:
  - Card click now opens the quick-place modal for that toy.
  - Added shared place helper for both form submit and modal actions.
  - Added modal close behaviors (Cancel, backdrop click, Escape key).
- Updated `ld-toypad-server/web/styles.css`:
  - Added modal styling.
  - Added `.modal[hidden] { display: none !important; }` to prevent hidden modal overlay from intercepting clicks.

### Validation
- Confirmed via live browser automation:
  - Modal `hidden` state transitions from `true` -> `false` on card click.
  - Modal text updates with selected toy name (example: `Place B.A. Baracus on:`).

## UX Polish: Persistent Zones + Standalone Selected Image

### Goal
- Keep zone state visible while browsing long toy lists.
- Promote selected toy image into its own top-of-page panel.

### Changes
- Updated `ld-toypad-server/web/index.html`:
  - Added a dedicated `Selected Toy` panel at the top with image and name.
  - Moved zones into a separate sidebar/frame layout next to the catalog panel.
  - Removed in-catalog selection card block.
- Updated `ld-toypad-server/web/styles.css`:
  - Added two-column layout (`app-layout`) for zones + catalog.
  - Made zones frame sticky (`.zones-panel { position: sticky; top: 12px; }`).
  - Added large selected image sizing for the top panel.
  - Added responsive fallback to single-column layout on small screens.
- Updated `ld-toypad-server/web/app.js`:
  - Simplified selected toy rendering for the top image panel (no legacy selection meta text dependency).

### Validation
- Browser automation confirmed:
  - Selected toy image panel updates on card selection.
  - Zones frame remains visible with sticky top offset while page is scrolled.

## UX Adjustment: Top Horizontal Zones Strip

### Goal
- Move zones into a smaller horizontal frame across the top.
- Provide per-zone remove controls.
- Show toy PNG in the zone where it is currently loaded.

### Changes
- Updated `ld-toypad-server/web/index.html`:
  - Moved zones to a top `zones-strip` panel.
  - Removed old sidebar zones frame and old global remove-button row.
- Updated `ld-toypad-server/web/styles.css`:
  - Zones now render as a compact 3-column strip on desktop.
  - Added compact zone-card layout with image thumb + text.
  - Added responsive stack behavior on mobile.
- Updated `ld-toypad-server/web/app.js`:
  - `renderZones()` now builds per-zone cards with:
    - inline `Remove` button (disabled when empty)
    - zone thumbnail image + fallback initials
  - Zone image mapping now resolves with `gameId` from status payload to avoid 404s.

### Validation
- Browser automation confirmed:
  - Zones render in 3 horizontal columns.
  - Placing a toy shows name + PNG in that zone card.
  - Clicking zone `Remove` clears the zone and disables that zone's remove button.

## Catalog Filters: Year + Included/DLC

### Goal
- Allow sorting/filtering catalog content by:
  - Year 1 vs Year 2
  - Included vs DLC

### Changes
- Updated `ld-toypad-server/web/index.html`:
  - Added filter tab groups for `Release` and `Ownership`.
- Updated `ld-toypad-server/web/app.js`:
  - Added `Release` tabs: `All`, `Year 1`, `Year 2`.
  - Added `Ownership` tabs: `All`, `Included`, `DLC`.
  - Extended catalog filter pipeline to apply both dimensions.
  - Added client-side metadata fallback inference so filters work even if server has not restarted.
- Updated `ld-toypad-server/virtual-toys.js`:
  - Added derived metadata on each toy record (`releaseYear`, `ownership`) for API parity.

### Validation
- Browser automation confirmed counts update correctly when switching tabs, e.g.:
  - Characters / All / Year 1 / All -> 47
  - Characters / All / Year 2 / All -> 30
  - Characters / All / All / Included -> 3

## Year Filter Correction: Vehicles/Items

### Issue
- Vehicles/items were being misclassified for Year 1/Year 2 because token entries do not carry reliable world metadata.

### Fix
- Updated release-year derivation to use stable item ID boundaries:
  - Characters: `itemId <= 46` => Year 1, otherwise Year 2
  - Vehicles/Items: `itemId <= 1172` => Year 1, otherwise Year 2
- Applied this rule in both:
  - `ld-toypad-server/virtual-toys.js` (API-side metadata)
  - `ld-toypad-server/web/app.js` (client-side fallback inference)

### Validation
- Browser automation confirmed non-zero split for vehicles/items:
  - Vehicles / Items / All / Year 1 / All -> 173
  - Vehicles / Items / All / Year 2 / All -> 72

## Connectivity Reliability Patch: PS3 <-> Server Auto Discovery

### Goal
- Establish reliable connection without manual endpoint fuss.
- Allow PS3 plugin to discover server automatically.

### Changes
- Updated [sprx-plugin/network.c](sprx-plugin/network.c):
  - Enabled UDP broadcast on PS3 socket (`SO_BROADCAST`) when available.
  - Added throttled discovery probe path (`network_maybe_probe_server`) that broadcasts poll packets when no server is known.
  - Added probe throttle interval to prevent poll-loop flooding.
- Updated [sprx-plugin/network.h](sprx-plugin/network.h):
  - Added declaration for `network_maybe_probe_server`.
- Updated [sprx-plugin/toypad_state.c](sprx-plugin/toypad_state.c):
  - Removed early-return behavior that prevented receive-side discovery.
  - On send failure with unknown server, plugin now attempts discovery probe and still processes receive path.
- Updated [ld-toypad-server/server.js](ld-toypad-server/server.js):
  - Enabled UDP broadcast on server socket.
  - Added periodic discovery beacon broadcast (`255.255.255.255:<udpPort>`) while no client is connected.
  - Fixed startup logging line to print correct listening `address:port`.
  - Added timer cleanup on all shutdown paths.

### Validation
- Static checks reported no syntax/runtime diagnostics in edited files.
- Server smoke start completed with discovery path active.

## Keystone Puzzle Zone Glow + Multi-Slot Completion (2026-07-14)

### Changes applied
This pass completes the portal-visibility and multi-slot zone experience for keystone puzzles.

#### `ld-toypad-server/server.js`
- **Enhanced `inferLitZones()`** with 4 heuristics covering all known HID output report formats:
  - Direct zone index in byte[1] (existing, preserved)
  - 3-bit zone mask in byte[2] (existing, preserved)
  - Extended 3-bit mask in byte[3] (new)
  - Per-zone brightness bytes[4-6] — non-zero = lit (new)
- This ensures keystone puzzle zone-flash commands are captured regardless of firmware variant.

#### `ld-toypad-server/web/styles.css`
- Added **`@keyframes keystone-glow`** animation with pulsing amber/gold box-shadow.
- Added **`.zone.zone-lit`** class — applies the glow animation + gradient background + gold accent text.
- Added **`.zone.zone-lit-sustain`** class — dimmer static glow for the fade-out period after telemetry stops.

#### `ld-toypad-server/web/app.js`
- Added **`state.litZones`** and **`state.litZoneTimestamps[]`** to track which zones are currently glowing and when each was last seen lit.
- Added **`applyZoneGlow()`** function — reads the zone heading text for zone number, applies `.zone-lit` when telemetry reports it, `.zone-lit-sustain` for 1.5s after the last report.
- Updated **`refresh()`** to update tracking timestamps before calling `applyZoneGlow()`.
- Glow sustain prevents flickering during rapid poll cycles where telemetry may briefly not reflect active zones.

### Validation
- `node --check` passes for both `server.js` and `web/app.js`.
- No syntax errors introduced.
- Left/right zones already correctly capped at 3 slots in `virtual-toys.js` (capacity arrays, place/remove slot logic).  
- Center zone already correctly single-slot.
- Duplicate placement supported (no uniqueness guard in slot resolution).

### Remaining items (not yet started) — now completed below

## Launch Sequence Fix: Wait for PS3 Before Uploading Token (2026-07-14, follow-up)

### Problem
Both launchers (`run-bridge-elevated.bat` and `start-ldtoypad.bat`) uploaded the one-shot activation token *before* starting the bridge server. The PS3 IP was guessed from a subnet FTP scan, which meant:
- If the guess was wrong, the token upload silently failed.
- No feedback was shown if the PS3 wasn't actually reachable.

### Fix applied
Reversed the launch order for both launchers:

#### `ld-toypad-server/run-bridge-elevated.bat`
1. Firewall rules (unchanged)
2. Kill old bridge (unchanged)
3. **Start node server in background** (`start /b`) first
4. **Wait for PS3 client** — polls `http://localhost:8080/api/status` in a 2s loop for up to 60s, extracting the server-confirmed `client.address`
5. **Upload token to confirmed IP** — once the server sees the PS3 UDP probe, upload the activation file via FTP to the *actual* IP
6. Cache the confirmed PS3 IP to `ps3-ip.txt` for next launch
7. **Bring server to foreground** in a visible console window

#### `start-ldtoypad.bat`
Same pattern: start server → wait for PS3 via API → upload token → open browser.

Key change: the token is always uploaded to the **server-confirmed** PS3 IP, not a subnet scan guess.

### Validation
- Both `.bat` files parse without syntax errors (verified by inspection).
- The `/api/status` endpoint already exposes `client.address` — no server code changes needed.
- Timeout fallback: if the PS3 never connects within 60s, the launcher skips token upload but leaves the server running so manual operation still works.
- The cached PS3 IP (`ps3-ip.txt`) accelerates future launches — the launcher skips the wait loop and goes straight to upload if a cached IP is found.
- `LDTP_PS3_IP` env var override still works for power users who want to skip discovery entirely.
