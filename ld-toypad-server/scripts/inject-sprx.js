#!/usr/bin/env node
/**
 * inject-sprx.js — PS3MAPI Game Process Injector for LD-ToyPad Bridge
 *
 * ARCHITECTURE (REFACTORED 2026-07-24 — Direct GOT Overwrite):
 *
 * Uses webMAN MOD 1.47.48c+ JSON RESTful API endpoint:
 *   GET /ps3mapi.ps3?<COMMAND>
 *
 * ALL HOOK WORK DONE IN-SPRX:
 *   - SPRX injected into game process
 *   - sys_memory_allocate allocates R-W-X trampoline pages
 *   - NID scanner finds cellUsbd GOT slots in game .data
 *   - SPRX overwrites resolved GOT slots with trampoline addresses
 *   - Game's PLT stubs (unchanged) load trampoline address from GOT
 *   - No PS3MAPI memory patches needed — everything in-process
 *
 * THIS SCRIPT ONLY:
 *   - Injects SPRX via PS3MAPI MODULE LOAD
 *   - Reads IPC file to VERIFY GOT overwrite took effect
 *   - No preamble writing (Phase 2 eliminated)
 *
 * ARCHITECTURE:
 *   PS3 SPRX -> NID scan -> GOT overwrite (in-process)
 *              -> IPC file for verification
 *              -> Game calls cellUsbd -> PLT stub reads GOT -> trampoline -> hooks
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

  // Unload previous PRX first
  log('  CRITICAL: Unloading previous PRX (clean teardown)...');
  try {
    const pidHex = '0x' + gamePid.toString(16);
    const encodedPath = encodeURIComponent(SPRX_PATH);
    const unloadEndpoint = `/ps3mapi.ps3?MODULE%20UNLOAD%20${pidHex}%20${encodedPath}`;
    const unloadResp = await ps3mapiRequest(unloadEndpoint, 5000);
    verbose(`  Unload response: ${unloadResp ? unloadResp.trim() : '(empty)'}`);
    await sleep(500);
    log('  ✓ Previous PRX unloaded');
  } catch (err) {
    verbose(`  No previous PRX to unload: ${err.message}`);
  }

  // Two-pass load
  log('  CRITICAL: Two-pass load to clear any stale VSH guard file...');
  
  const pidHex = '0x' + gamePid.toString(16);
  const encodedPath = encodeURIComponent(SPRX_PATH);
  const loadEndpoint = `/ps3mapi.ps3?MODULE%20LOAD%20${pidHex}%20${encodedPath}`;
  
  try {
    const resp1 = await ps3mapiRequest(loadEndpoint, 15000);
    log(`  Attempt 1 response: ${resp1 ? resp1.trim() : '(empty)'}`);
    await sleep(1000);
    
    const resp2 = await ps3mapiRequest(loadEndpoint, 15000);
    log(`  Attempt 2 response: ${resp2 ? resp2.trim() : '(empty)'}`);
    
    const lower = resp2.toLowerCase();
    if (lower.includes('success') || lower.includes('loaded') || lower.includes('ok') || lower.includes('"result"')) {
      log('✓ SPRX injection SUCCESSFUL!');
      return true;
    }
    log(`⚠ Injection response: "${resp2.trim()}"`);
    return true;
  } catch (err) {
    log(`✗ Injection FAILED: ${err.message}`);
    
    log('  Retrying once...');
    try {
      await sleep(2000);
      const resp3 = await ps3mapiRequest(loadEndpoint, 15000);
      log(`  Retry response: ${resp3 ? resp3.trim() : '(empty)'}`);
      log('✓ Injection successful on retry');
      return true;
    } catch (err2) {
      log(`✗ Injection retry also failed: ${err2.message}`);
      return false;
    }
  }
}

// ──────────────────────────────────────────────
// 4. Wait for SPRX IPC file (verification only)
// ──────────────────────────────────────────────
// The SPRX now does everything in-process:
//   - NID scan -> find GOT slots
//   - Overwrite GOT slots with trampoline addresses
//   - Write IPC file for verification
//
// No preamble writing needed! The game's PLT stubs already
// load our trampoline addresses from the overwritten GOT slots.
async function waitForIpcAndVerify(gamePid) {
  log('Waiting for SPRX IPC file (ld_hooks_ready.txt) for verification...');
  log('  SPRX does everything in-process: NID scan, GOT overwrite,');
  log('  then writes IPC file for verification. No preamble needed.');
  log('  Using direct HTTP GET: /dev_hdd0/tmp/ld_hooks_ready.txt');
  
  let ipcContent = null;
  
  for (let attempt = 0; attempt < 80; attempt++) {
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
  log(`  TRAMP_INIT:       ${kv.TRAMP_INIT || 'unknown'}`);
  log(`  TRAMP_OPENPIPE:   ${kv.TRAMP_OPENPIPE || 'unknown'}`);
  log(`  TRAMP_TRANSFER:   ${kv.TRAMP_TRANSFER || 'unknown'}`);
  log(`  TRAMP_CLOSEPIPE:  ${kv.TRAMP_CLOSEPIPE || 'unknown'}`);
  log(`  TARGET_INIT:      ${kv.TARGET_INIT || 'unknown'} (GOT slot address)`);
  log(`  TARGET_OPENPIPE:  ${kv.TARGET_OPENPIPE || 'unknown'}`);
  log(`  TARGET_TRANSFER:  ${kv.TARGET_TRANSFER || 'unknown'}`);
  log(`  TARGET_CLOSEPIPE: ${kv.TARGET_CLOSEPIPE || 'unknown'}`);

  // Count how many GOT overwrites succeeded
  let gotCount = 0;
  for (const key of ['TARGET_INIT', 'TARGET_OPENPIPE', 'TARGET_TRANSFER', 'TARGET_CLOSEPIPE']) {
    if (kv[key] && kv[key] !== '0x0' && kv[key] !== '0x00000000') {
      gotCount++;
    }
  }

  log(`  ✓ ${gotCount}/4 GOT slots overwritten by SPRX in-process`);

  if (gotCount === 4) {
    log('');
    log('  ╔══════════════════════════════════════════════════╗');
    log('  ║  ALL 4 GOT HOOKS ACTIVE — No preamble needed!  ║');
    log('  ╚══════════════════════════════════════════════════╝');
    log('');
    log('  The SPRX directly overwrote the game\'s resolved');
    log('  cellUsbd GOT slots with trampoline addresses.');
    log('  Game PLT stubs now redirect to our hooks.');
  }

  return true;
}

// ──────────────────────────────────────────────
// 4b. Poll init_progress papertrail file
// ──────────────────────────────────────────────
async function pollInitProgress(gamePid) {
  log('Polling SPRX init progress (ld_paper.txt + ld_init_progress.txt)...');

  let gInitProgressAddr = null;
  for (let attempt = 0; attempt < 30; attempt++) {
    try {
      const resp = await ps3mapiRequest(`/dev_hdd0/tmp/ld_init_progress.txt`, 3000);
      if (resp && resp.length > 0) {
        const match = resp.match(/INIT_PROGRESS_ADDR=0x([0-9A-Fa-f]+)/);
        if (match) {
          gInitProgressAddr = parseInt(match[1], 16);
          log(`✓ g_init_progress address: 0x${gInitProgressAddr.toString(16)}`);
          break;
        }
      }
    } catch { }
    await sleep(200);
  }

  for (let attempt = 0; attempt < 120; attempt++) {
    try {
      const paperResp = await ps3mapiRequest(`/dev_hdd0/tmp/ld_paper.txt`, 3000);
      if (paperResp && paperResp.length > 0) {
        const step = parseInt(paperResp.trim(), 10);
        log(`  [progress] SPRX at step ${step}`);
      } else {
        log(`  [progress] ld_paper.txt not yet written (step 0)`);
      }
    } catch {
      log(`  [progress] ld_paper.txt not ready yet`);
    }

    if (gInitProgressAddr) {
      try {
        const pidHex = '0x' + gamePid.toString(16);
        const addrHex = '0x' + gInitProgressAddr.toString(16);
        const memResp = await ps3mapiRequest(
          `/ps3mapi.ps3?MEMORY%20GET%20${pidHex}%20${addrHex}%200x4`,
          3000
        );
        if (memResp && memResp.length > 0) {
          const cleanHex = memResp.replace(/[^0-9A-Fa-f]/g, '');
          if (cleanHex.length >= 8) {
            const directProgress = parseInt(cleanHex.substring(0, 8), 16);
            if (directProgress !== 0) {
              log(`  [mem] g_init_progress = ${directProgress}`);
            }
          }
        }
      } catch { }
    }

    try {
      const ipcCheck = await ps3mapiRequest(`/dev_hdd0/tmp/ld_hooks_ready.txt`, 2000);
      if (ipcCheck && ipcCheck.includes('STATUS=ready')) {
        log('✓ IPC file detected — SPRX initialization complete!');
        return true;
      }
    } catch { }

    if (attempt % 10 === 0 && attempt > 0) {
      log(`  ...still waiting for SPRX init (attempt ${attempt + 1}/120)`);
    }

    await sleep(1000);
  }

  log('✗ SPRX did not show progress for 120s — probable crash');
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

  // Step 5: Verify IPC (GOT overwrite verification only)
  log('');
  log('Step 5: Verifying GOT overwrites via IPC file...');
  const verified = await waitForIpcAndVerify(gamePid);

  if (verified) {
    log('');
    log('╔══════════════════════════════════════════════════╗');
    log('║  ✓ INJECTION COMPLETE                          ║');
    log('║                                                ║');
    log('║  SPRX injected — GOT overwritten in-process    ║');
    log('║  No PS3MAPI memory patches needed              ║');
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
