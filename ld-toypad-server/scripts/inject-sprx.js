#!/usr/bin/env node
/**
 * inject-sprx.js — PS3MAPI Game Process Injector for LD-ToyPad Bridge
 *
 * ARCHITECTURE (REFACTORED 2026-07-27 - OPD-Based libusbd.sprx Hooking):
 *
 * Uses webMAN MOD 1.47.48c+ JSON RESTful API endpoint:
 *   GET /ps3mapi.ps3?<COMMAND>
 *
 * ALL HOOK WORK DONE IN-SPRX:
 *   - SPRX injected into game process
 *   - sys_memory_allocate allocates R-W-X trampoline pages
 *   - OPD resolver extracts real libusbd.sprx code addresses from
 *     SPRX resolved stub imports (via TOC+GOT slot read)
 *   - Addresses written to IPC file for Node.js verification
 *   - Node.js writes 4-insn preambles into libusbd.sprx .text
 *     via PS3MAPI MEMORY SET (pending implementation)
 *
 * THIS SCRIPT:
 *   - Injects SPRX via PS3MAPI MODULE LOAD
 *   - Polls boot log for init status
 *   - Reads IPC file to verify resolved addresses
 *
 * ARCHITECTURE:
 *   SPRX -> OPD import -> TOC+GOT read -> real libusbd addr -> IPC
 *   Node.js -> PS3MAPI preamble write at libusbd code addresses
 *
 * URL REFERENCE (webMAN MOD 1.47.48c+):
 *   Process detect:  /ps3mapi.ps3?PROCESS%20GETCURRENTPID
 *   Module load:     /ps3mapi.ps3?MODULE%20LOAD%200x{PID}%20{path}
 *   Module unload:   /ps3mapi.ps3?MODULE%20UNLOAD%200x{PID}%20{path}
 *   Memory read:     /ps3mapi.ps3?MEMORY%20GET%20{PID}%20{addr}%20{size}
 *   File access:     /dev_hdd0/tmp/filename.txt (direct filesystem)
 *
 * USAGE:
 *   node inject-sprx.js [options]
 *
 * OPTIONS:
 *   --ps3-ip <ip>     PS3 IP address (default: from ps3-ip.txt)
 *   --sprx-path <p>   Path to SPRX on PS3 (default: /dev_hdd0/plugins/ldtoypad.sprx)
 *   --wait <sec>      Seconds to wait after game detected (default: 60)
 *   --poll <ms>       Poll interval for game detection (default: 2000)
 *   --no-inject       Scan only, do not inject
 *   --verbose, -v     Verbose logging
 *   --port <num>      PS3MAPI HTTP port (default: 80)
 *   --help, -h        Show this help
 */

const http = require('http');
const path = require('path');
const fs = require('fs');

// ──────────────────────────────────────────────
// Configuration
// ──────────────────────────────────────────────
const PS3_IP = (() => {
  const idx = process.argv.indexOf('--ps3-ip');
  if (idx !== -1 && process.argv[idx + 1]) return process.argv[idx + 1];
  try {
    return fs.readFileSync(path.join(__dirname, '..', 'ps3-ip.txt'), 'utf8').trim();
  } catch { return null; }
})();

const SPRX_PATH = (() => {
  const idx = process.argv.indexOf('--sprx-path');
  if (idx !== -1 && process.argv[idx + 1]) return process.argv[idx + 1];
  return '/dev_hdd0/plugins/ldtoypad.sprx';
})();

const WAIT_SECONDS = (() => {
  const idx = process.argv.indexOf('--wait');
  if (idx !== -1 && process.argv[idx + 1]) return parseInt(process.argv[idx + 1], 10);
  return 60;
})();

const POLL_MS = (() => {
  const idx = process.argv.indexOf('--poll');
  if (idx !== -1 && process.argv[idx + 1]) return parseInt(process.argv[idx + 1], 10);
  return 2000;
})();

const PS3MAPI_PORT = (() => {
  const idx = process.argv.indexOf('--port');
  if (idx !== -1 && process.argv[idx + 1]) return parseInt(process.argv[idx + 1], 10);
  return 80;
})();

const NO_INJECT = process.argv.includes('--no-inject');
const VERBOSE = process.argv.includes('--verbose') || process.argv.includes('-v');

// ──────────────────────────────────────────────
// PS3MAPI HTTP Helper
// ──────────────────────────────────────────────
function ps3mapiRequest(endpoint, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    const req = http.get(`http://${PS3_IP}:${PS3MAPI_PORT}${endpoint}`, (res) => {
      let data = '';
      res.on('data', chunk => data += chunk);
      res.on('end', () => resolve(data));
    });
    req.on('error', (err) => reject(err));
    req.setTimeout(timeoutMs, () => { req.destroy(); reject(new Error('timeout')); });
  });
}

// ──────────────────────────────────────────────
// Logging
// ──────────────────────────────────────────────
function log(msg) {
  const ts = new Date().toISOString().replace(/T/, ' ').replace(/\..+/, '');
  console.log(`[${ts}] ${msg}`);
}
function verbose(msg) {
  if (VERBOSE) log(`[VERB] ${msg}`);
}

// ──────────────────────────────────────────────
// 1. Detect Game Process
// ──────────────────────────────────────────────
async function detectGame() {
  log(`Detecting game on ${PS3_IP}:${PS3MAPI_PORT} (poll every ${POLL_MS}ms)...`);
  log('  Using webMAN 1.47.48c+ JSON API: /ps3mapi.ps3?PROCESS%20GETCURRENTPID');
  log('  Game PID detected when: PID >= 0x1010000 (VSH transition or already running)');
  
  let attemptsWithoutVsh = 0;
  
  while (true) {
    try {
      const resp = await ps3mapiRequest('/ps3mapi.ps3?PROCESS%20GETCURRENTPID');
      verbose(`Process list response:\n${resp || '(empty)'}`);
      
      const gamePid = parseJsonPid(resp);
      
      if (gamePid !== null && gamePid >= 0x1010000) {
        const pidHex = '0x' + gamePid.toString(16);
        log(`✓ Game detected! PID=${pidHex} (${gamePid})`);
        return gamePid;
      }

      // Count consecutive polls without finding game (to show diagnostic)
      if (gamePid !== null && gamePid < 0x1010000) {
        attemptsWithoutVsh++;
        if (attemptsWithoutVsh === 1) {
          log(`  PS3 is at PID=${pidHex} (VSH/XMB) — waiting for game to launch...`);
        }
      }
    } catch (err) {
      verbose(`Poll failed: ${err.message}`);
    }
    
    await sleep(POLL_MS);
  }
}

/**
 * verifyGameProcess removed — PS3MAPI MEMORY GET cannot reliably read
 * cross-process memory at 0x00010000 in the game's address space.
 * Instead, detectGame() uses VSH→game PID transition + range filter.
 */

function parseJsonPid(resp) {
  if (!resp) return null;
  
  try {
    const json = JSON.parse(resp);
    const pidStr = (json && json.response) ? String(json.response) :
                   (json && json.result) ? String(json.result) : null;
    if (pidStr) {
      const hexMatch = pidStr.match(/0x([0-9a-fA-F]+)/);
      if (hexMatch) {
        const pid = parseInt(hexMatch[1], 16);
        if (pid > 0 && pid !== 0x10005) {
          verbose(`  PID candidate from JSON: ${pidStr} → parsed ${pid} (0x${pid.toString(16)})`);
          return pid;
        }
      }
    }
  } catch { }

  const hexMatch = resp.match(/0x([0-9a-fA-F]+)/);
  if (hexMatch) {
    const pid = parseInt(hexMatch[1], 16);
    if (pid !== 0x10005 && pid > 0x10000) {
      verbose(`  PID candidate from raw hex: 0x${hexMatch[1]} → ${pid}`);
      return pid;
    }
  }
  
  const numMatch = resp.match(/(\d+)/);
  if (numMatch) {
    const pid = parseInt(numMatch[1], 10);
    if (pid > 0x10000) return pid;
  }
  
  return null;
}

// ──────────────────────────────────────────────
// 2. Wait for Game to Stabilize
// ──────────────────────────────────────────────
async function waitForGame(gamePid) {
  log(`Waiting ${WAIT_SECONDS}s for game to initialize...`);
  
  const start = Date.now();
  const totalMs = WAIT_SECONDS * 1000;
  
  while (Date.now() - start < totalMs) {
    const elapsed = Date.now() - start;
    const remaining = Math.ceil((totalMs - elapsed) / 1000);
    
    if (remaining > 0 && remaining % 10 === 0 && remaining <= WAIT_SECONDS) {
      log(`  ...${remaining}s remaining`);
    }
    
    await sleep(1000);
  }
  
  log(`✓ Game stabilization period complete (${WAIT_SECONDS}s)`);
}

// ──────────────────────────────────────────────
// 3. Inject SPRX via PS3MAPI JSON API
// ──────────────────────────────────────────────
async function injectSprx(gamePid) {
  log(`Injecting SPRX into game PID=0x${gamePid.toString(16)}...`);
  log(`  SPRX path: ${SPRX_PATH}`);
  log('  Using webMAN 1.47.48c+ JSON API: /ps3mapi.ps3?MODULE%20LOAD/UNLOAD...');

  // Unload previous PRX first (retry up to 3 times)
  log('  CRITICAL: Unloading previous PRX (clean teardown)...');
  let unloaded = false;
  for (let retry = 0; retry < 3 && !unloaded; retry++) {
    try {
      const pidHex = '0x' + gamePid.toString(16);
      const encodedPath = encodeURIComponent(SPRX_PATH);
      const unloadEndpoint = `/ps3mapi.ps3?MODULE%20UNLOAD%20${pidHex}%20${encodedPath}`;
      const unloadResp = await ps3mapiRequest(unloadEndpoint, 5000);
      verbose(`  Unload response: ${unloadResp ? unloadResp.trim() : '(empty)'}`);
      await sleep(500);
      log('  ✓ Previous PRX unloaded');
      unloaded = true;
    } catch (err) {
      if (retry < 2) {
        log(`  Unload attempt ${retry + 1} failed: ${err.message} — retrying...`);
        await sleep(1000);
      } else {
        verbose(`  No previous PRX to unload after 3 attempts: ${err.message}`);
      }
    }
  }

  // Delete stale papertrail files BEFORE injection so we can detect
  // whether module_start actually runs (vs reading old files).
  log('  Deleting stale papertrail files...');
  for (const f of ['/dev_hdd0/tmp/ld_paper.txt', '/dev_hdd0/tmp/ld_init_progress.txt', '/dev_hdd0/tmp/ld_hooks_ready.txt']) {
    try { await ps3mapiRequest(f + '?delete', 2000); } catch {}
  }
  await sleep(200);
  log('  ✓ Stale files cleared');

  // Single-pass load with up to 3 retries on PS3MAPI failure
  log('  Loading SPRX via PS3MAPI (up to 3 retries)...');
  
  const pidHex = '0x' + gamePid.toString(16);
  const encodedPath = encodeURIComponent(SPRX_PATH);
  const loadEndpoint = `/ps3mapi.ps3?MODULE%20LOAD%20${pidHex}%20${encodedPath}`;
  
  for (let retry = 0; retry < 3; retry++) {
    try {
      const resp = await ps3mapiRequest(loadEndpoint, 15000);
      log(`  Load response (attempt ${retry + 1}/3): ${resp ? resp.trim() : '(empty)'}`);
      
      const lower = resp.toLowerCase();
      if (lower.includes('success') || lower.includes('loaded') || lower.includes('ok') || lower.includes('"result"') || lower.includes('"code"')) {
        log('✓ SPRX injection SUCCESSFUL!');
        return true;
      }
      log(`⚠ Unexpected response: "${resp.trim()}"`);
      return true;
    } catch (err) {
      log(`  Attempt ${retry + 1}/3 failed: ${err.message}`);
      if (retry < 2) {
        log(`  Retrying in 2s...`);
        await sleep(2000);
      }
    }
  }
  
  log('✗ Injection FAILED after 3 attempts');
  return false;
}

// ──────────────────────────────────────────────
// 4. Wait for SPRX IPC file (verification)
// ──────────────────────────────────────────────
// The SPRX resolves real libusbd.sprx code addresses via OPD imports
// and writes them to an IPC file. Node.js reads the file for
// verification. Preamble writing into libusbd.sprx is pending.
async function waitForIpcAndVerify(gamePid) {
  log('Waiting for SPRX IPC file (ld_hooks_ready.txt) for verification...');
  log('  SPRX resolves libusbd.sprx addresses via OPD + TOC+GOT,');
  log('  writes IPC file. Preamble installation pending.');
  log('  Using direct HTTP GET: /dev_hdd0/plugins/ld_hooks_ready.txt');
  
  let ipcContent = null;
  
  for (let attempt = 0; attempt < 80; attempt++) {
    try {
      const resp = await ps3mapiRequest(
        `/dev_hdd0/plugins/ld_hooks_ready.txt`,
        3000
      );
      
      if (resp && resp.length > 0 && resp.includes('STATUS=ready')) {
        ipcContent = resp;
        log('✓ IPC file found! Parsing addresses...');
        break;
      }
    } catch { }

    if (attempt % 10 === 0) {
      log(`  ...waiting for IPC file (attempt ${attempt + 1}/80)`);
    }
    await sleep(1000);
  }
  
  if (!ipcContent) {
    log('✗ IPC file not found after 80 seconds. SPRX may have crashed.');
    log('  Check /dev_hdd0/plugins/ldtoypad_boot.log on PS3.');
    return false;
  }
  
  const kv = {};
  for (const line of ipcContent.split('\n')) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const eqIdx = trimmed.indexOf('=');
    if (eqIdx === -1) continue;
    const key = trimmed.substring(0, eqIdx).trim();
    const val = trimmed.substring(eqIdx + 1).trim();
    kv[key] = val;
  }
  
  verbose(`IPC parsed: ${JSON.stringify(kv, null, 2)}`);
  
  log(`  Trampoline base:  ${kv.TRAMP_BASE || 'unknown'}`);
  log(`  TRAMP_OPENPIPE:   ${kv.TRAMP_OPENPIPE || 'unknown'}`);
  log(`  TRAMP_TRANSFER:   ${kv.TRAMP_TRANSFER || 'unknown'}`);
  log(`  TRAMP_CLOSEPIPE:  ${kv.TRAMP_CLOSEPIPE || 'unknown'}`);
  log(`  TRAMP_GETDEVDESC: ${kv.TRAMP_GETDEVDESC || 'unknown'}`);
  log(`  TRAMP_CTRLXFER:   ${kv.TRAMP_CTRLXFER || 'unknown'}`);

  // ──────────────────────────────────────────────
  // 4a. Smart Probe: find libusbd base by scanning
  //     for "cellUsbd_Library" string at B + 0x94EC
  // ──────────────────────────────────────────────
  const TARGET_HEX = '63656c6c557362645f4c696272617279';  // "cellUsbd_Library"
  const MODULE_INFO_OFFSET = 0x94EC;  // vaddr of name string in sceModuleInfo

  log('');
  log('Scanning memory for libusbd.sprx base (Smart Probe)...');
  log(`  String "cellUsbd_Library" at base + 0x${MODULE_INFO_OFFSET.toString(16)}`);

  const scanRanges = [
    { start: 0x02000000, end: 0x03000000 },
    { start: 0x30000000, end: 0x31000000 },
  ];

  let libusbdBase = null;

  for (const range of scanRanges) {
    if (libusbdBase) break;
    for (let base = range.start; base < range.end; base += 0x10000) {
      const probeAddr = base + MODULE_INFO_OFFSET;
      const probeHex = '0x' + probeAddr.toString(16).toUpperCase();
      const pidHex = '0x' + gamePid.toString(16);

      try {
        const resp = await ps3mapiRequest(
          `/ps3mapi.ps3?MEMORY%20GET%20${pidHex}%20${probeHex}%2016`, 1000
        );
        // Extract hex from JSON response: {"response": "68656C6C6F..."}
        const match = resp.match(/"response":\s*"([0-9a-fA-F]+)"/);
        const hexStr = match ? match[1].toLowerCase() : '';

        if (hexStr === TARGET_HEX) {
          libusbdBase = base;
          log(`  \u2713 FOUND at 0x${base.toString(16).toUpperCase()}!`);
          break;
        }
      } catch (e) {
        // Timeout or unmapped — safely skip
      }
    }
    if (!libusbdBase) {
      log(`  Range 0x${range.start.toString(16)}-0x${range.end.toString(16)}: not found`);
    }
  }

  if (!libusbdBase) {
    log('\u2717 Smart Probe could not find libusbd.sprx base.');
    log('  libusbd may be at an unexpected address range.');
    return false;
  }

  // ──────────────────────────────────────────────
  // 4b. Compute TARGET_* addresses and write preambles
  // ──────────────────────────────────────────────
  const LIBUSBD_OFFSET_OPENPIPE       = 0x00000244;
  const LIBUSBD_OFFSET_INTERRUPT_XFER = 0x000004B4;
  const LIBUSBD_OFFSET_CLOSEPIPE      = 0x00000380;
  const LIBUSBD_OFFSET_GET_DEV_DESC   = 0x0000061C;
  const LIBUSBD_OFFSET_CONTROL_XFER   = 0x000007C8;

  const TARGET_OPENPIPE   = libusbdBase + LIBUSBD_OFFSET_OPENPIPE;
  const TARGET_TRANSFER   = libusbdBase + LIBUSBD_OFFSET_INTERRUPT_XFER;
  const TARGET_CLOSEPIPE  = libusbdBase + LIBUSBD_OFFSET_CLOSEPIPE;
  const TARGET_GETDEVDESC = libusbdBase + LIBUSBD_OFFSET_GET_DEV_DESC;
  const TARGET_CTRLXFER   = libusbdBase + LIBUSBD_OFFSET_CONTROL_XFER;

  log('');
  log('Computed libusbd.sprx hook targets:');
  log(`  TARGET_OPENPIPE:   0x${TARGET_OPENPIPE.toString(16).toUpperCase()}`);
  log(`  TARGET_TRANSFER:   0x${TARGET_TRANSFER.toString(16).toUpperCase()}`);
  log(`  TARGET_CLOSEPIPE:  0x${TARGET_CLOSEPIPE.toString(16).toUpperCase()}`);
  log(`  TARGET_GETDEVDESC: 0x${TARGET_GETDEVDESC.toString(16).toUpperCase()}`);
  log(`  TARGET_CTRLXFER:   0x${TARGET_CTRLXFER.toString(16).toUpperCase()}`);

  // Build 4-instruction preamble for each target:
  //   lis r11, tramp_hi
  //   ori r11, r11, tramp_lo
  //   mtctr r11
  //   bctr
  function buildPreamble(trampAddr) {
    const addr = parseInt(trampAddr, 16);
    const hi = (addr >> 16) & 0xFFFF;
    const lo = addr & 0xFFFF;
    // lis r11, hi  = 0x3D60 | hi
    const lis = 0x3D600000 | hi;
    // ori r11, r11, lo = 0x616B | lo
    const ori = 0x616B0000 | lo;
    // mtctr r11 = 0x7D6903A6
    // bctr = 0x4E800420
    return [lis, ori, 0x7D6903A6, 0x4E800420];
  }

  const targets = [
    { name: 'OpenPipe',       addr: TARGET_OPENPIPE,   trampKey: 'TRAMP_OPENPIPE' },
    { name: 'InterruptXfer',  addr: TARGET_TRANSFER,   trampKey: 'TRAMP_TRANSFER' },
    { name: 'ClosePipe',      addr: TARGET_CLOSEPIPE,  trampKey: 'TRAMP_CLOSEPIPE' },
    { name: 'GetDevDesc',     addr: TARGET_GETDEVDESC, trampKey: 'TRAMP_GETDEVDESC' },
    { name: 'ControlXfer',    addr: TARGET_CTRLXFER,   trampKey: 'TRAMP_CTRLXFER' },
  ];

  log('');
  log('Writing 4-instruction preambles via PS3MAPI MEMORY SET...');
  let preamblesWritten = 0;

  for (const t of targets) {
    const trampAddr = kv[t.trampKey];
    if (!trampAddr || trampAddr === '0x00000000' || trampAddr === '0x0') {
      log(`  SKIP ${t.name}: no trampoline address`);
      continue;
    }

    const preamble = buildPreamble(trampAddr);
    // PS3MAPI MEMORY SET: write 4 words (16 bytes) at target address
    // Format: /ps3mapi.ps3?MEMORY%20SET%20<PID>%20<ADDR>%20<HEXBYTES>
    const hexBytes = preamble.map(w => w.toString(16).padStart(8, '0')).join('');
    const pidHex = '0x' + gamePid.toString(16);
    const addrHex = '0x' + t.addr.toString(16).toUpperCase();

    try {
      const endpoint = `/ps3mapi.ps3?MEMORY%20SET%20${pidHex}%20${addrHex}%20${hexBytes}`;
      await ps3mapiRequest(endpoint, 5000);
      log(`  \u2713 ${t.name}: preamble at 0x${t.addr.toString(16).toUpperCase()} -> trampoline`);
      preamblesWritten++;
    } catch (err) {
      log(`  \u2717 ${t.name}: MEMORY SET failed: ${err.message}`);
    }
  }

  log('');
  if (preamblesWritten === 5) {
    log('\u2713 All 5 preambles installed! cellUsbd hooks ACTIVE.');
  } else {
    log(`\u26A0 ${preamblesWritten}/5 preambles written.`);
  }

  return true;
}

// ──────────────────────────────────────────────
// 4b. Poll init_progress papertrail file
// ──────────────────────────────────────────────
async function pollInitProgress(gamePid) {
  log('Checking SPRX boot log for init status...');
  log('  Reading /dev_hdd0/plugins/ldtoypad_boot.log');

  let lastLine = '';
  for (let attempt = 0; attempt < 120; attempt++) {
    try {
      const resp = await ps3mapiRequest('/dev_hdd0/plugins/ldtoypad_boot.log', 3000);
      if (resp && resp.length > 0) {
        const lines = resp.split('\n').filter(l => l.trim().length > 0);
        const latest = lines[lines.length - 1].trim();

        // Only log when the line changes
        if (latest !== lastLine && latest.length > 0) {
          lastLine = latest;
          log(`  [boot] ${latest}`);
        }

        // Check for success markers
        if (resp.includes('ToyPad LDD REGISTERED')) {
          log('✓ LDD driver registered — ToyPad will be claimed!');
        }
        if (resp.includes('Entering main loop')) {
          log('✓ SPRX main loop active — UDP bridge running on port 28472');
          return true;
        }
        if (resp.includes('FATAL:')) {
          log('✗ SPRX reported fatal error — check boot log');
          return false;
        }
      }
    } catch {
      if (attempt === 0) {
        log('  ...boot log not available yet, waiting...');
      }
    }

    if (attempt % 15 === 14) {
      log(`  ...still waiting (${attempt + 1}s)`);
    }
    await sleep(1000);
  }

  log('✗ SPRX did not reach main loop after 120s');
  return false;
}

// ──────────────────────────────────────────────
// Utility
// ──────────────────────────────────────────────

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

function printUsage() {
  console.log(`
Usage: node inject-sprx.js [options]

Options:
  --ps3-ip <ip>       PS3 IP address (default: from ps3-ip.txt)
  --sprx-path <path>  Path to SPRX on PS3 (default: /dev_hdd0/plugins/ldtoypad.sprx)
  --wait <sec>        Seconds to wait after game detected (default: 60)
  --poll <ms>         Poll interval for game detection (default: 2000)
  --no-inject         Scan only, do not inject
  --verbose, -v       Verbose logging
  --port <num>        PS3MAPI HTTP port (default: 80)
  --help, -h          Show this help

Examples:
  node inject-sprx.js --ps3-ip 192.168.0.47
  node inject-sprx.js --ps3-ip 192.168.0.47 --wait 45 --verbose
  node inject-sprx.js --no-inject --ps3-ip 192.168.0.47
`);
}

// ──────────────────────────────────────────────
// Main
// ──────────────────────────────────────────────
async function main() {
  console.log('╔══════════════════════════════════════════════════╗');
  console.log('║   LD-ToyPad Bridge — PS3MAPI Game Injector     ║');
  console.log('╚══════════════════════════════════════════════════╝');
  console.log('');
  
  if (!PS3_IP) {
    console.error('✗ PS3 IP not provided. Use --ps3-ip or create ps3-ip.txt');
    printUsage();
    process.exit(1);
  }
  
  log(`PS3 IP:     ${PS3_IP}`);
  log(`SPRX path:  ${SPRX_PATH}`);
  log(`Wait time:  ${WAIT_SECONDS}s`);
  log(`Poll rate:  ${POLL_MS}ms`);
  log(`Inject:     ${NO_INJECT ? 'NO (scan only)' : 'YES'}`);
  log('');
  
  // Step 1: Verify PS3MAPI connectivity
  log('Step 1: Verifying PS3MAPI connectivity...');
  try {
    const root = await ps3mapiRequest('/', 3000);
    const webmanMatch = root.match(/webMAN\s*MOD\s*([\d.]+)/i);
    if (webmanMatch) {
      log(`✓ webMAN MOD ${webmanMatch[1]} detected on ${PS3_IP}`);
    } else if (root.includes('webMAN')) {
      log('✓ webMAN detected');
    } else {
      log('⚠ PS3 responded but webMAN MOD not confirmed');
    }
  } catch (err) {
    log(`✗ PS3 not reachable: ${err.message}`);
    log('  Make sure the PS3 is on and webMAN MOD is running.');
    process.exit(1);
  }
  
  // Step 2: Detect game
  log('');
  log('Step 2: Detecting game process...');
  const gamePid = await detectGame();
  
  // Step 3: Wait for game to stabilize
  log('');
  log('Step 3: Waiting for game initialization...');
  await waitForGame(gamePid);
  
  // Step 4: Inject SPRX
  log('');
  if (NO_INJECT) {
    log('Step 4: SKIPPING injection (--no-inject flag)');
    log('');
    log('To inject, run without --no-inject flag');
    return;
  }
  
  log('Step 4: Injecting SPRX into game process...');
  const injected = await injectSprx(gamePid);
  
  if (!injected) {
    log('');
    log('╔══════════════════════════════════════════════════╗');
    log('║  ✗ INJECTION FAILED                            ║');
    log('║                                                ║');
    log('║  Check:                                        ║');
    log('║  1. SPRX exists at: ' + SPRX_PATH);
    log('║  2. PS3MAPI is enabled in webMAN MOD settings  ║');
    log('║  3. Game is at the "Connect Toy Pad" screen    ║');
    log('╚══════════════════════════════════════════════════╝');
    return;
  }
  
  // Step 4b: Poll init_progress
  log('');
  log('Step 4b: Polling SPRX init_progress address...');
  await pollInitProgress(gamePid);

  // Step 5: Verify IPC (libusbd.sprx address verification)
  log('');
  log('Step 5: Verifying libusbd.sprx addresses via IPC file...');
  const verified = await waitForIpcAndVerify(gamePid);

  if (verified) {
    log('');
    log('╔══════════════════════════════════════════════════╗');
    log('║  ✓ INJECTION COMPLETE                          ║');
    log('║                                                ║');
    log('║  SPRX injected — libusbd.sprx addrs resolved   ║');
    log('║  Preamble install pending Node.js impl          ║');
    log('║                                                ║');
    log('║  CellUsbd hooks now active in game process.    ║');
    log('║                                                ║');
    log('║  Check the server terminal for:                ║');
    log('║    RX type=0x01 zone=1 seq=N  (poll packets)  ║');
    log('║    RX type=0x04 zone=1 seq=N  (data out)      ║');
    log('╚══════════════════════════════════════════════════╝');
  } else {
    log('');
    log('╔══════════════════════════════════════════════════╗');
    log('║  ⚠ SPRX LOADED BUT IPC NOT FOUND               ║');
    log('║                                                ║');
    log('║  The SPRX is loaded but may have crashed during ║');
    log('║  hook initialization. Check boot.log.          ║');
    log('╚══════════════════════════════════════════════════╝');
  }
}

// Run
main().catch(err => {
  console.error(`Fatal error: ${err.message}`);
  console.error(err.stack);
  process.exit(1);
});
