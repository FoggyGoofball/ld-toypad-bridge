#!/usr/bin/env node
/**
 * inject-sprx.js — PS3MAPI Game Process Injector for LD-ToyPad Bridge
 *
 * TWO-PHASE HOOK INSTALLATION (REFACTORED 2026-07-21):
 *
 * Uses webMAN MOD 1.47.48c+ JSON RESTful API endpoint:
 *   GET /ps3mapi.ps3?<COMMAND>
 *
 * PHASE 1 (SPRX-side):
 *   - SPRX injected into game process via /ps3mapi.ps3?MODULE%20LOAD...
 *   - NID scan finds cellUsbd function addresses
 *   - sys_memory_allocate allocates R-W-X trampoline pages
 *   - Original 4 instructions copied + branch-back written
 *   - IPC file written to /dev_hdd0/tmp/ld_hooks_ready.txt
 *
 * PHASE 2 (Node.js-side, THIS SCRIPT):
 *   - Polls for IPC file via direct HTTP GET /dev_hdd0/tmp/ld_hooks_ready.txt
 *   - Parses target and wrapper addresses
 *   - Writes 4-instruction preamble (lis/ori/mtctr/bctr) to each
 *     target function via PS3MAPI MEMORY SET command
 *   - R-X .text segment protection bypassed by PS3MAPI kernel module
 *
 * ARCHITECTURE:
 *   ┌──────────┐   HTTP (port 80)    ┌────────────┐
 *   │  PS3      │ ←───────────────── │  inject    │
 *   │  webMAN   │                    │  -sprx.js  │
 *   │  MOD      │                    └─────┬──────┘
 *   └────┬─────┘                           │
 *        │ 1) MODULE LOAD                  │
 *        ▼                                 │
 *   ┌──────────┐  2) writes IPC file      │
 *   │ SPRX     │ ──────────────────────→  │
 *   │ (game)   │                          │
 *   └──────────┘  3) MEMORY SET           │
 *        │        ←─────────────────────  │
 *        │  preamble (4 insns)            │
 *        ▼                                │
 *   ┌──────────┐  cellUsbd hooks         │
 *   │ GAME     │ ────────────────→  server│
 *   │ process  │  UDP:28472              │
 *   └──────────┘                         │
 *
 * URL MIGRATION NOTES (webMAN MOD 1.47.48c+):
 *   OLD: /ps3mapi_process                    →  400 Bad Request
 *   NEW: /ps3mapi.ps3?PROCESS%20GETCURRENTPID
 *   OLD: /ps3mapi_process?pid=X&load_prx=... →  400 Bad Request
 *   NEW: /ps3mapi.ps3?MODULE%20LOAD%200xX%20/path
 *   OLD: /cpursx.ps3?/write_process...       →  Non-standard
 *   NEW: /ps3mapi.ps3?MEMORY%20SET%200xX%200xADDR%20HEX
 *   OLD: /cpursx.ps3?/read_process?path=...   →  Non-standard
 *   NEW: /dev_hdd0/tmp/filename.txt          →  Direct filesystem
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
 *   --server-start    Also start the UDP bridge server automatically
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
// Uses webMAN MOD 1.47.48c+ JSON RESTful API:
//   GET /ps3mapi.ps3?PROCESS%20GETCURRENTPID
// Returns JSON: { "result": "0x01030000" } or similar
async function detectGame() {
  log(`Detecting game on ${PS3_IP}:${PS3MAPI_PORT} (poll every ${POLL_MS}ms)...`);
  log('  Using webMAN 1.47.48c+ JSON API: /ps3mapi.ps3?PROCESS%20GETCURRENTPID');
  
  while (true) {
    try {
      const resp = await ps3mapiRequest('/ps3mapi.ps3?PROCESS%20GETCURRENTPID');
      verbose(`Process list response:\n${resp || '(empty)'}`);
      
      const gamePid = parseJsonPid(resp);
      if (gamePid !== null) {
        log(`✓ Game detected! PID=0x${gamePid.toString(16)} (${gamePid})`);
        return gamePid;
      }
    } catch (err) {
      verbose(`Poll failed: ${err.message}`);
    }
    
    await sleep(POLL_MS);
  }
}

// Parse JSON response from /ps3mapi.ps3?PROCESS GETCURRENTPID
// Actual observed format: { "response": "0x0000000001010200" }
//   - Key is "response" (not "result")
//   - Value is 64-bit hex with leading zeros, e.g. 0x0000000001010200
// Fallback: raw hex string like "0x01030000"
function parseJsonPid(resp) {
  if (!resp) return null;
  
  // Try JSON parse first (official webMAN MOD 1.47.48c+ format)
  try {
    const json = JSON.parse(resp);
    // Try "response" key first (actual webMAN MOD API format)
    const pidStr = (json && json.response) ? String(json.response) :
                   (json && json.result) ? String(json.result) : null;
    if (pidStr) {
      const hexMatch = pidStr.match(/0x([0-9a-fA-F]+)/);
      if (hexMatch) {
        const pid = parseInt(hexMatch[1], 16);
        // PID 0x10005 = VSH (XMB), PID must be > 0x10000
        if (pid > 0 && pid !== 0x10005) {
          verbose(`  PID candidate from JSON: ${pidStr} → parsed ${pid} (0x${pid.toString(16)})`);
          return pid;
        }
      }
    }
  } catch {
    // Not JSON, try raw formats below
  }
  
  // Fallback: any hex string in the response (handles 64-bit leading zeros)
  const hexMatch = resp.match(/0x([0-9a-fA-F]+)/);
  if (hexMatch) {
    const pid = parseInt(hexMatch[1], 16);
    // VSH (XMB) PID, skip it
    if (pid !== 0x10005 && pid > 0x10000) {
      verbose(`  PID candidate from raw hex: 0x${hexMatch[1]} → ${pid}`);
      return pid;
    }
  }
  
  // Fallback: decimal number string
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
// Uses webMAN MOD 1.47.48c+ JSON RESTful API:
//   UNLOAD: GET /ps3mapi.ps3?MODULE%20UNLOAD%200x{PID}%20{path}
//   LOAD:   GET /ps3mapi.ps3?MODULE%20LOAD%200x{PID}%20{path}
// Returns JSON: { "result": "success" } or similar
async function injectSprx(gamePid) {
  log(`Injecting SPRX into game PID=0x${gamePid.toString(16)}...`);
  log(`  SPRX path: ${SPRX_PATH}`);
  log('  Using webMAN 1.47.48c+ JSON API: /ps3mapi.ps3?MODULE%20LOAD/UNLOAD...');

  // Step 3a: Unload previous PRX (if any)
  //
  // CRITICAL: The network_init() regression is caused by an orphaned socket
  // from a previous SPRX injection that never ran module_stop. The old worker
  // thread still holds g_net.socket_fd open, so bind() fails with EADDRINUSE.
  //
  // UDP has no TIME_WAIT state — the port is held by the live thread, not
  // the kernel. SO_REUSEADDR would force-bind but cause nondeterministic
  // packet routing between the ghost SPRX and new SPRX (severe packet loss).
  //
  // The correct fix: explicitly unload the previous PRX via PS3MAPI before
  // loading the new one. This triggers module_stop → worker join → socket
  // close → sys_net_finalize → clean slate.
  log('  CRITICAL: Unloading previous PRX (clean teardown)...');
  try {
    const pidHex = '0x' + gamePid.toString(16);
    const encodedPath = encodeURIComponent(SPRX_PATH);
    const unloadEndpoint = `/ps3mapi.ps3?MODULE%20UNLOAD%20${pidHex}%20${encodedPath}`;
    const unloadResp = await ps3mapiRequest(unloadEndpoint, 5000);
    verbose(`  Unload response: ${unloadResp ? unloadResp.trim() : '(empty)'}`);
    await sleep(500);  // Wait for module_stop to complete
    log('  ✓ Previous PRX unloaded');
  } catch (err) {
    // If there was no previous PRX loaded, the unload fails harmlessly
    verbose(`  No previous PRX to unload: ${err.message}`);
  }

  // Step 3b: Load new PRX
  const pidHex = '0x' + gamePid.toString(16);
  const encodedPath = encodeURIComponent(SPRX_PATH);
  const endpoint = `/ps3mapi.ps3?MODULE%20LOAD%20${pidHex}%20${encodedPath}`;
  verbose(`Endpoint: ${endpoint}`);
  
  try {
    const resp = await ps3mapiRequest(endpoint, 15000);
    log(`  Response: ${resp ? resp.trim() : '(empty)'}`);
    
    // Check for success indicators in JSON response
    if (resp) {
      const lower = resp.toLowerCase();
      if (lower.includes('success') || lower.includes('loaded') || lower.includes('ok') || lower.includes('"result"')) {
        log('✓ SPRX injection SUCCESSFUL!');
        return true;
      }
      log(`⚠ Injection response: "${resp.trim()}"`);
      return true;
    } else {
      log('⚠ Empty response — injection may have succeeded');
      return true;
    }
  } catch (err) {
    log(`✗ Injection FAILED: ${err.message}`);
    
    log('  Retrying once...');
    try {
      await sleep(2000);
      const resp2 = await ps3mapiRequest(endpoint, 15000);
      log(`  Retry response: ${resp2 ? resp2.trim() : '(empty)'}`);
      log('✓ Injection successful on retry');
      return true;
    } catch (err2) {
      log(`✗ Injection retry also failed: ${err2.message}`);
      return false;
    }
  }
}


// ──────────────────────────────────────────────
// 4. Wait for SPRX IPC file + Write Preambles
// ──────────────────────────────────────────────
// Uses webMAN MOD 1.47.48c+ endpoints:
//   IPC POLL:  GET /dev_hdd0/tmp/ld_hooks_ready.txt  (direct filesystem)
//   MEM WRITE: GET /ps3mapi.ps3?MEMORY%20SET%200x{PID}%200x{ADDR}%20{HEX}
async function waitForIpcAndInstall(gamePid) {
  log('Waiting for SPRX to write IPC file (ld_hooks_ready.txt)...');
  log('  SPRX will NID-scan, allocate trampolines, copy original');
  log('  instructions, and write IPC file on /dev_hdd0/tmp/.');
  log('  This should take < 1 second after injection.');
  log('  Using direct HTTP GET: /dev_hdd0/tmp/ld_hooks_ready.txt');
  
  let ipcContent = null;
  
  // Poll for IPC file (up to 15 seconds)
  // Uses direct filesystem access via webMAN HTTP server
  for (let attempt = 0; attempt < 30; attempt++) {
    try {
      const resp = await ps3mapiRequest(
        `/dev_hdd0/tmp/ld_hooks_ready.txt`,
        3000
      );
      
      if (resp && resp.length > 0 && resp.includes('STATUS=ready')) {
        ipcContent = resp;
        log('✓ IPC file found! Parsing addresses...');
        break;
      }
    } catch {
      // File not ready yet
    }
    
    if (attempt % 5 === 0) {
      log(`  ...waiting for IPC file (attempt ${attempt + 1}/30)`);
    }
    await sleep(500);
  }
  
  if (!ipcContent) {
    log('✗ IPC file not found after 15 seconds. SPRX may have crashed.');
    log('  Check /dev_hdd0/plugins/ldtoypad_boot.log on PS3.');
    return false;
  }
  
  // Parse IPC content into key-value map
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
  
  // Show discovered addresses
  log(`  Trampoline base: ${kv.TRAMP_BASE || 'unknown'}`);
  log(`  Target INIT:     ${kv.INIT_ADDR || 'unknown'} -> wrapper ${kv.INIT_WRAP || 'unknown'}`);
  log(`  Target OPEN:     ${kv.OPENPIPE_ADDR || 'unknown'} -> wrapper ${kv.OPENPIPE_WRAP || 'unknown'}`);
  log(`  Target XFER:     ${kv.TRANSFER_ADDR || 'unknown'} -> wrapper ${kv.TRANSFER_WRAP || 'unknown'}`);
  log(`  Target CLOSE:    ${kv.CLOSEPIPE_ADDR || 'unknown'} -> wrapper ${kv.CLOSEPIPE_WRAP || 'unknown'}`);
  
  // Build and install preambles for each target
  const targets = [
    { name: 'cellUsbdInit',       target: kv.INIT_ADDR,       wrapper: kv.INIT_WRAP },
    { name: 'cellUsbdOpenPipe',   target: kv.OPENPIPE_ADDR,   wrapper: kv.OPENPIPE_WRAP },
    { name: 'cellUsbdTransfer',   target: kv.TRANSFER_ADDR,   wrapper: kv.TRANSFER_WRAP },
    { name: 'cellUsbdClosePipe',  target: kv.CLOSEPIPE_ADDR,  wrapper: kv.CLOSEPIPE_WRAP },
  ];
  
  for (const t of targets) {
    if (!t.target || !t.wrapper || t.target === '0x0' || t.wrapper === '0x0') {
      log(`  ⚠ Skipping ${t.name}: missing address`);
      continue;
    }
    
    const targetAddr = parseInt(t.target, 16);
    const wrapperAddr = parseInt(t.wrapper, 16);
    
    if (isNaN(targetAddr) || isNaN(wrapperAddr)) {
      log(`  ⚠ Skipping ${t.name}: invalid address format`);
      continue;
    }
    
    log(`  Installing preamble on ${t.name} @ 0x${targetAddr.toString(16)} -> wrapper 0x${wrapperAddr.toString(16)}`);
    
    // Build 4-instruction preamble:
    // [0] lis  r11, hi16(wrapper_addr)    0x3D60xxxx
    // [1] ori  r11, r11, lo16(wrapper)    0x616Bxxxx
    // [2] mtctr r11                       0x7D6B03A6
    // [3] bctr                            0x4E800420
    const preamble = Buffer.alloc(16); // 4 instructions × 4 bytes
    preamble.writeUInt32BE((0x3D60 << 16) | ((wrapperAddr >> 16) & 0xFFFF), 0);  // lis r11
    preamble.writeUInt32BE((0x616B << 16) | (wrapperAddr & 0xFFFF), 4);          // ori r11,r11
    preamble.writeUInt32BE(0x7D6B03A6, 8);  // mtctr r11
    preamble.writeUInt32BE(0x4E800420, 12); // bctr
    
    // Write via PS3MAPI MEMORY SET command (webMAN MOD 1.47.48c+ JSON API)
    // Ring 0 level, bypasses R-X .text segment protection
    const hexData = preamble.toString('hex').toUpperCase();
    const pidHex = '0x' + gamePid.toString(16);
    const addrHex = '0x' + targetAddr.toString(16);
    const writeEndpoint = `/ps3mapi.ps3?MEMORY%20SET%20${pidHex}%20${addrHex}%20${hexData}`;
    
    verbose(`  Write endpoint: ${writeEndpoint}`);
    
    try {
      const writeResp = await ps3mapiRequest(writeEndpoint, 5000);
      verbose(`  Write response: ${writeResp ? writeResp.trim() : '(empty)'}`);
      log(`  ✓ Preamble installed on ${t.name}`);
    } catch (err) {
      log(`  ✗ Failed to write preamble on ${t.name}: ${err.message}`);
    }
    
    await sleep(100); // Small delay between writes
  }
  
  log('✓ All preambles installed!');
  log('  Game will now route Toy Pad USB traffic via hooks.');
  return true;
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
  
  // Step 4: Inject SPRX (Phase 1 — SPRX prepares trampolines)
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
  
  // Step 5: Wait for IPC + install preambles (Phase 2 — Node.js writes via PS3MAPI)
  log('');
  log('Step 5: Polling for SPRX IPC and installing preambles...');
  const installed = await waitForIpcAndInstall(gamePid);
  
  if (installed) {
    log('');
    log('╔══════════════════════════════════════════════════╗');
    log('║  ✓ FULL INSTALLATION COMPLETE                  ║');
    log('║                                                ║');
    log('║  Phase 1: SPRX injected — trampolines ready    ║');
    log('║  Phase 2: Preambles installed via Ring 0       ║');
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
    log('║  ⚠ SPRX INJECTED BUT PREAMBLES FAILED          ║');
    log('║                                                ║');
    log('║  The SPRX is loaded but hooks not installed.   ║');
    log('║  The game should continue normally (no crash). ║');
    log('║  Check /dev_hdd0/plugins/ldtoypad_boot.log.    ║');
    log('╚══════════════════════════════════════════════════╝');
  }
}

// Run
main().catch(err => {
  console.error(`Fatal error: ${err.message}`);
  console.error(err.stack);
  process.exit(1);
});
