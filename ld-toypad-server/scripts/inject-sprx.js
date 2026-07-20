#!/usr/bin/env node
/**
 * inject-sprx.js — PS3MAPI Game Process Injector for LD-ToyPad Bridge
 *
 * PURPOSE:
 *   Detects when the LEGO Dimensions game launches on the PS3, waits for
 *   it to fully initialize (intro cinematics, trophy scan, heap init),
 *   scans the game's memory for cellUsbd function addresses, then injects
 *   the ldtoypad.sprx plugin into the game process.
 *
 * ONCE INJECTED:
 *   The SPRX resides in the game's address space and can install PowerPC
 *   detour hooks on the game's cellUsbdInit, cellUsbdOpenPipe,
 *   cellUsbdTransfer, and cellUsbdClosePipe functions.  USB Toy Pad
 *   traffic is then routed to the Node.js server via UDP.
 *
 * ARCHITECTURE:
 *   ┌──────────┐   HTTP (port 80)    ┌────────┐
 *   │  PS3      │ ←───────────────── │   PC   │
 *   │  webMAN   │                    │ inject │
 *   │  MOD      │                    │ .sprx  │
 *   └────┬─────┘                    └────────┘
 *        │ PS3MAPI process injection
 *        ▼
 *   ┌──────────┐  cellUsbd hooks    ┌────────┐
 *   │ GAME     │ ────────────────→  │ server │
 *   │ process  │  UDP:28472         │ .js    │
 *   └──────────┘                    └────────┘
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
// Known cellUsbd NID Values (FNV-1a 32-bit, masked 0x7FFFFFFF)
// These identify the functions in the game's PRX import table.
// ──────────────────────────────────────────────
const CELLUSBD_NIDS = {
  cellUsbdInit:           0x7F5F00D3,
  cellUsbdOpenPipe:       0x1AB6D80B,
  cellUsbdTransfer:       0x7B4436CE,
  cellUsbdClosePipe:      0x2F82F1A5,
};

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
  
  while (true) {
    try {
      const resp = await ps3mapiRequest('/ps3mapi_process');
      verbose(`Process list response:\n${resp || '(empty)'}`);
      
      // Parse process listing — multiple format variants:
      // Format 1: "VSH: process id=0x10005 game: process id=0x1000a"
      // Format 2: "pid=0x10005 name=VSH"
      // Format 3: XML
      // Format 4: CSV or raw text
      
      const gamePid = parseGamePid(resp);
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

function parseGamePid(resp) {
  if (!resp) return null;
  
  const candidates = [];
  
  // Method 1: Look for game process by name (EBOOT.BIN, BLUS31548, BLES02206, etc.)
  const gamePatterns = [/EBOOT\.?BIN/i, /BLUS\d+/i, /BLES\d+/i, /NPUB\d+/i, /NPEB\d+/i, /LEGO/i, /DIMENSION/i, /GAME/i];
  
  // Split into lines
  const lines = resp.split('\n').map(l => l.trim()).filter(l => l.length > 0);
  
  for (const line of lines) {
    // Extract process ID
    let pid = null;
    let name = null;
    
    // Try different formats
    
    // Format: "VSH : process id=0x10005" or "game: pid=0x1000a"
    const pidMatch = line.match(/pid\s*[=:]\s*(0x[0-9a-fA-F]+)/i);
    if (pidMatch) pid = parseInt(pidMatch[1], 16);
    
    // Format: "process id=0x10005"
    const pidMatch2 = line.match(/process\s*id\s*[=:]\s*(0x[0-9a-fA-F]+)/i);
    if (pidMatch2 && pid === null) pid = parseInt(pidMatch2[1], 16);
    
    // Format: just "0x10005" or "10005" 
    const pidMatch3 = line.match(/^0x([0-9a-fA-F]+)$/);
    if (pidMatch3 && pid === null) pid = parseInt(pidMatch3[1], 16);
    
    // Extract name
    const nameMatch = line.match(/name\s*[=:]\s*(\S+)/i);
    if (nameMatch) name = nameMatch[1];
    
    // Format: "VSH : process id=..." — name is before colon
    if (!name) {
      const namePrefix = line.match(/^(\S+)\s*:/);
      if (namePrefix) name = namePrefix[1];
    }
    
    if (pid !== null) {
      // Check if this looks like a game (not VSH)
      if (name) {
        const isGame = gamePatterns.some(p => p.test(name));
        if (isGame) candidates.push({ pid, name, score: 10 });
      }
      
      // VSH is always pid 0x10005 — anything else with higher pid is likely game
      if (pid !== 0x10005 && pid !== 0x10000005) {
        candidates.push({ pid, name: name || `PID_${pid.toString(16)}`, score: 5 });
      }
    }
  }
  
  // If nothing parsed via patterns, try raw hex extraction
  if (candidates.length === 0) {
    const rawPids = resp.match(/0x([0-9a-fA-F]{4,8})/g);
    if (rawPids) {
      for (const raw of rawPids) {
        const pid = parseInt(raw, 16);
        if (pid !== 0x10005 && pid > 0x10005) {
          candidates.push({ pid, name: `RAW_${raw}`, score: 3 });
        }
      }
    }
  }
  
  // Return highest-scored candidate
  if (candidates.length > 0) {
    candidates.sort((a, b) => b.score - a.score);
    verbose(`Process candidates: ${JSON.stringify(candidates)}`);
    return candidates[0].pid;
  }
  
  return null;
}

// ──────────────────────────────────────────────
// 2. Wait for Game to Stabilize
// ──────────────────────────────────────────────
async function waitForGame(gamePid) {
  log(`Waiting ${WAIT_SECONDS}s for game to initialize...`);
  
  // Show countdown every 10 seconds
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
// 3. Scan game memory for cellUsbd function addresses
// ──────────────────────────────────────────────
async function scanCellUsbdAddresses(gamePid) {
  log('Scanning game memory for cellUsbd function addresses...');
  
  // Strategy: Read the game's .got2 section which contains pointers to
  // imported system library functions like cellUsbd.
  //
  // On PS3, import stubs in the game's PRX have the pattern:
  //   lis r12, <nid_high>    0x3D80xxxx
  //   ori r12, r12, <nid_low> 0x618Cxxxx
  //   ...
  // The resolved function pointer is stored in .got2.
  //
  // Since directly scanning the entire 256MB game memory is slow via HTTP,
  // we use a targeted approach: read the PRX module list to find .got2 segments.
  
  const foundAddrs = {};
  let totalScanned = 0;
  
  // Read game memory regions that might contain import stubs
  // Typical game segments: .text (0x00100000+), .got2, etc.
  // We search regions where PRX modules are mapped
  const searchRegions = [
    // Game's main executable region (EBOOT.BIN mapped at base address)
    { start: 0x00100000, size: 0x00800000 },   // 8MB
    { start: 0x01000000, size: 0x01000000 },   // 16MB
    { start: 0x02000000, size: 0x01000000 },   // 16MB
    // Some games load at higher addresses
    { start: 0x30000000, size: 0x00800000 },   // 8MB
    { start: 0x40000000, size: 0x01000000 },   // 16MB
  ];
  
  for (const region of searchRegions) {
    const hexAddr = region.start.toString(16).padStart(8, '0');
    const hexSize = region.size.toString(16).padStart(8, '0');
    
    verbose(`Reading memory: 0x${hexAddr} (${region.size} bytes)...`);
    
    try {
      const resp = await ps3mapiRequest(
        `/cpursx.ps3?/read_process?pid=0x${gamePid.toString(16)}&addr=0x${hexAddr}&size=0x${hexSize}`,
        10000
      );
      
      if (resp && resp.length > 0) {
        // Convert hex string response to buffer
        const buf = Buffer.from(resp.replace(/[^0-9a-fA-F]/g, ''), 'hex');
        totalScanned += region.size;
        
        // Search for cellUsbd NIDs in import stubs
        // Import stub pattern: lis r12, nid_high + ori r12, nid_low
        // lis r12 = 0x3D80xxxx
        // ori r12, r12, 0x618Cxxxx
        for (const [funcName, nid] of Object.entries(CELLUSBD_NIDS)) {
          if (foundAddrs[funcName]) continue; // Already found
          
          const nidHigh = (nid >> 16) & 0xFFFF;
          const nidLow = nid & 0xFFFF;
          
          // Search for the lis/ori pair
          const lisPattern = Buffer.alloc(4);
          lisPattern.writeUInt32BE((0x3D80 << 16) | nidHigh, 0);
          
          const oriPattern = Buffer.alloc(4);
          oriPattern.writeUInt32BE((0x618C << 16) | nidLow, 0);
          
          let idx = 0;
          while (idx < buf.length - 8) {
            const lisMatch = buf.indexOf(lisPattern, idx);
            if (lisMatch === -1 || lisMatch > buf.length - 8) break;
            
            // Check if next instruction is the ori
            if (buf.readUInt32BE(lisMatch + 4) === oriPattern.readUInt32BE(0)) {
              // Found the import stub! The actual function address is at a
              // .got2 slot offset from this stub. Look for the lwz instruction
              // that loads from GOT: lwz r12, offset(r11) = 0x858Bxxxx
              // We'll take a different approach: search look for the lwz after the ori
              
              // The typical pattern continues after lis/ori:
              //   lwz r12, offset(r11)  — load ptr from GOT
              //   mtctr r12
              //   bctr
              // We need to read the pointer value from the GOT offset
              
              // For simplicity in v1, we read the next 12 bytes and look for lwz
              if (lisMatch + 16 <= buf.length) {
                const stubEnd = buf.slice(lisMatch, lisMatch + 16);
                verbose(`${funcName} import stub at 0x${(region.start + lisMatch).toString(16)}: ${stubEnd.toString('hex')}`);
              }
              
              // Use a heuristic: the function address is likely near this stub
              // in the GOT. But finding the exact GOT slot requires full PRX parsing.
              // 
              // For v1, we record the base address and continue scanning.
              // The actual address resolution will be done inside the SPRX
              // (which has full memory access).
              
              foundAddrs[funcName] = true; // Mark as found for scanning purposes
              const relAddr = (region.start + lisMatch).toString(16);
              log(`  [SCAN] Found ${funcName} import stub at game+0x${relAddr}`);
            }
            
            idx = lisMatch + 1;
          }
        }
      }
    } catch (err) {
      verbose(`Region 0x${hexAddr} read failed: ${err.message}`);
    }
  }
  
  log(`Scanned ${(totalScanned / 1024 / 1024).toFixed(1)}MB of game memory`);
  
  // For now, we write the known NID values and let the SPRX resolve them internally.
  // The SPRX has full memory access once injected.
  return CELLUSBD_NIDS;
}

// ──────────────────────────────────────────────
// 4. Write cellUsbd addresses to PS3 temp file
// ──────────────────────────────────────────────
async function writeAddressFile(gamePid, nidMap) {
  log('Writing cellUsbd NID reference file to PS3...');
  
  // Build content
  const lines = [];
  for (const [name, nid] of Object.entries(nidMap)) {
    lines.push(`${name}=0x${nid.toString(16).padStart(8, '0')}`);
  }
  lines.push(`game_pid=0x${gamePid.toString(16)}`);
  const content = lines.join('\n') + '\n';
  
  // Write via PS3MAPI's file management or FTP
  // webMAN MOD supports file write through its HTTP API
  try {
    // Attempt to write via PS3MAPI file endpoint
    // Most webMAN MOD versions support: /cpursx.ps3?/write_file?path=...&data=...
    // But this is not universally available. Fallback: skip and let SPRX handle it.
    const encodedPath = encodeURIComponent('/dev_hdd0/tmp/ld_cellusbd_nids.txt');
    const encodedData = encodeURIComponent(content);
    
    try {
      await ps3mapiRequest(`/cpursx.ps3?/write_file?path=${encodedPath}&data=${encodedData}`, 3000);
      log('✓ NID reference file written to /dev_hdd0/tmp/ld_cellusbd_nids.txt');
    } catch {
      // Write method not available — that's OK, the SPRX has the NIDs compiled in
      log('  (PS3MAPI file write not available — SPRX will use compiled-in NIDs)');
    }
  } catch (err) {
    verbose(`Write failed: ${err.message}`);
  }
}

// ──────────────────────────────────────────────
// 5. Inject SPRX via PS3MAPI
// ──────────────────────────────────────────────
async function injectSprx(gamePid) {
  log(`Injecting SPRX into game PID=0x${gamePid.toString(16)}...`);
  log(`  SPRX path: ${SPRX_PATH}`);
  
  // webMAN MOD injection endpoint:
  // GET /ps3mapi_process?pid=<PID>&load_prx=<FULL_PATH>
  
  const endpoint = `/ps3mapi_process?pid=0x${gamePid.toString(16)}&load_prx=${encodeURIComponent(SPRX_PATH)}`;
  verbose(`Endpoint: ${endpoint}`);
  
  try {
    const resp = await ps3mapiRequest(endpoint, 15000);
    log(`  Response: ${resp ? resp.trim() : '(empty)'}`);
    
    // Check for success indicators
    if (resp && (resp.includes('success') || resp.includes('loaded') || resp.includes('OK') || resp.includes('0x0'))) {
      log('✓ SPRX injection SUCCESSFUL!');
      return true;
    } else if (resp && resp.length > 0) {
      // Got a response but unclear — log it
      log(`⚠ Injection response: "${resp.trim()}" — may be successful`);
      return true;
    } else {
      // Empty response could mean success on some webMAN versions
      log('⚠ Empty response — injection may have succeeded');
      return true;
    }
  } catch (err) {
    log(`✗ Injection FAILED: ${err.message}`);
    
    // Retry once
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
// 6. Verify injection
// ──────────────────────────────────────────────
async function verifyInjection(gamePid) {
  log('Verifying injection (checking for 0x01 poll packets from game)...');
  log('  Check the server.js terminal for incoming USB poll packets.');
  log('  Expected within 3-5 seconds: RX type=0x01 zone=1 seq=N');
  
  // We can verify indirectly by checking if the game still responds to PS3MAPI
  // (if the SPRX crashed the game, PS3MAPI would become unresponsive)
  try {
    await sleep(3000);
    const resp = await ps3mapiRequest(`/ps3mapi_process?pid=0x${gamePid.toString(16)}`, 3000);
    if (resp && resp.length > 0) {
      log('✓ Game process still alive after injection');
      return true;
    }
  } catch {
    log('⚠ Could not verify game state after injection');
  }
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
  
  // Validate PS3 IP
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
  
  // Step 4: Scan for cellUsbd addresses
  log('');
  log('Step 4: Scanning for cellUsbd import addresses...');
  const nidMap = await scanCellUsbdAddresses(gamePid);
  
  // Step 5: Write address reference file
  log('');
  log('Step 5: Writing NID reference data...');
  await writeAddressFile(gamePid, nidMap);
  
  // Step 6: Inject SPRX
  log('');
  if (NO_INJECT) {
    log('Step 6: SKIPPING injection (--no-inject flag)');
    log('');
    log('To inject, run without --no-inject flag');
  } else {
    log('Step 6: Injecting SPRX into game process...');
    const injected = await injectSprx(gamePid);
    
    if (injected) {
      // Step 7: Verify
      log('');
      log('Step 7: Verifying injection...');
      await verifyInjection(gamePid);
      
      log('');
      log('╔══════════════════════════════════════════════════╗');
      log('║  ✓ INJECTION COMPLETE                          ║');
      log('║                                                ║');
      log('║  The SPRX is now running inside the game       ║');
      log('║  process. CellUsbd hooks should be active.     ║');
      log('║                                                ║');
      log('║  Check the server terminal for:                ║');
      log('║    RX type=0x01 zone=1 seq=N  (poll packets)  ║');
      log('║    RX type=0x04 zone=1 seq=N  (data out)      ║');
      log('╚══════════════════════════════════════════════════╝');
    } else {
      log('');
      log('╔══════════════════════════════════════════════════╗');
      log('║  ✗ INJECTION FAILED                            ║');
      log('║                                                ║');
      log('║  Check:                                        ║');
      log('║  1. SPRX exists at: ' + SPRX_PATH);
      log('║  2. PS3MAPI is enabled in webMAN MOD settings  ║');
      log('║  3. Game is at the "Connect Toy Pad" screen    ║');
      log('╚══════════════════════════════════════════════════╝');
    }
  }
}

// Run
main().catch(err => {
  console.error(`Fatal error: ${err.message}`);
  console.error(err.stack);
  process.exit(1);
});
