# LD-ToyPad Bridge Handoff Changelog (2026-07-14, Session 2-5)

## Resume Marker: One-Shot Enable Token (Failsafe)

### What was working at handoff
- ✅ SPRX builds successfully with native PSL1GHT API (11,776 bytes)
- ✅ Browser UI fully functional (tabbed catalog, thumbnails, zone strip, keystone glow)
- ✅ Multi-slot zones (L=3, C=1, R=3) with duplicate support
- ✅ Launcher scripts (`start-ldtoypad.bat`, `run-bridge-elevated.bat`)
- ✅ PS3 IP caching (`ps3-ip.txt`)
- ✅ Extra LDD registration via `sysUsbdRegisterExtraLdd` (graceful fallback)

### What was broken / incomplete at handoff
- ❌ **No failsafe**: If the SPRX crash-locks the PS3 during development, the only recovery was safe mode, which is painful
- ❌ **Enable flag was persistent** — once FTP'd, it stayed on disk forever and the plugin always armed

---

## Changes Applied This Session

### 1. `sprx-plugin/main.c` — One-shot consumable enable token
- **File**: `sprx-plugin/main.c`, function `check_enable_flag()`
- Previously: `sysFsStat()` → return 1 if file exists
- Now: `sysFsStat()` → **`sysFsUnlink()`** to delete the file → return 1
- On the next boot the token is gone → plugin returns `SYS_PRX_START_OK` immediately
- A single PS3 power cycle is always enough to restore normal behaviour
- Compatible with any existing `ldtoypad.enable` file — it will be consumed on first boot

### 2. SPRX rebuilt (Session 5)
| Metric | Before | After |
|--------|--------|-------|
| SPRX size | 11,776 bytes | **11,824 bytes** |
| Change | — | `sysFsUnlink()` + `DEBUG_PRINT` (~48 bytes) |
| Errors | 0 | 0 |
| Warnings | PSL1GHT header warnings | Same (benign) |

---

## Failsafe Design

The one-shot token creates a safe development cycle:

```
┌─────────────┐     ┌──────────────────┐     ┌──────────────────┐
│ FTP enable   │ ──> │ Reboot PS3       │ ──> │ Plugin sees      │
│ flag to PS3  │     │ (token present)  │     │ token, DELETES   │
└─────────────┘     └──────────────────┘     │ it, arms thread   │
                                             └────────┬─────────┘
                                                      │
                                                      ▼
                                        ┌─────────────────────────┐
                                        │ Test / develop          │
                                        │ If crash → power cycle  │
                                        │ Token gone → safe boot  │
                                        └─────────────────────────┘
```

**No safe mode required. No USB re-flash. No FTP recovery.** Just power-cycle the PS3.

---

## Verification (Session 3 — still relevant)

| Check | Result |
|---|---|
| SPRX compiles with native PSL1GHT API | ✅ 11,824 bytes, 0 errors |
| Token consumed on boot (sysFsUnlink) | ✅ Code review pass |
| Next boot safe (token gone) | ✅ Logical — `sysFsStat` returns -1, return 0 |
| PS3 network reachable | ✅ `192.168.0.47` answers ping |
| webMAN HTTP (port 80) | ✅ `wMAN MOD 1.47.45` responds |

**Outstanding**: The SPRX still needs to be deployed to PS3 and tested with an actual game launch to verify the plugin lifecycle end-to-end.

---

## Key Files Summary

| File | Purpose | Last Modified |
|------|---------|--------------|
| `sprx-plugin/main.c` | PRX entry, native PSL1GHT API, **one-shot enable token** | 2026-07-14 (Session 5) |
| `sprx-plugin/ldd_driver.c` | Extra LDD registration via `sysUsbdRegisterExtraLdd` | 2026-07-14 (Session 4) |
| `sprx-plugin/network.c` | UDP transport, startup beacon + recv spin, server discovery | 2026-07-14 (Session 3) |
| `ldbuild.sh` | Windows→WSL build bridge | 2026-07-14 (Session 4) |
| `deploy-enable.ps1` | FTP enable flag to PS3 (create `ldtoypad.enable`) | 2026-07-14 |

---

## Build & Deploy Commands

```bash
# Build
wsl bash "/mnt/c/Users/Admin/source/repos/dimensions plugin/ldbuild.sh"

# Deploy SPRX to PS3
powershell -File ftp-deploy.ps1

# Arm the plugin (FTP enable flag)
powershell -File deploy-enable.ps1

# Reboot PS3 → plugin fires once → token consumed
# Next reboot → clean, normal PS3
```

---

## Handoff Checklist

- [x] Apply one-shot token consumption via `sysFsUnlink()`
- [x] Rebuild SPRX (11,824 bytes, 0 errors)
- [x] Update changelog
- [ ] Deploy SPRX to PS3 via FTP
- [ ] FTP enable flag → reboot → test plugin fires
- [ ] Reboot again → verify plugin stays dormant
