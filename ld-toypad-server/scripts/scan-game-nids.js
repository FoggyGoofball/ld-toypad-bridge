#!/usr/bin/env node
/**
 * scan-game-nids.js — PC-side NID scanner for LEGO Dimensions
 *
 * Reads game memory via PS3MAPI getmem to find cellUsbd import stubs.
 * The game binds cellUsbd functions through import stubs that look like:
 *   12 bytes: NID(4) + reserved(4) + GOT_ptr(4)
 *
 * Usage: node scan-game-nids.js [--ps3-ip <ip>] [--pid 0xNNNNN] [--verbose]
 */

const http = require('http');
const path = require('path');
const fs = require('fs');

const PS3_IP = (() => {
  const idx = process.argv.indexOf('--ps3-ip');
  if (idx !== -1 && process.argv[idx + 1]) return process.argv[idx + 1];
  try {
    return fs.readFileSync(path.join(__dirname, '..', 'ps3-ip.txt'), 'utf8').trim();
  } catch { return '192.168.0.47'; }
})();

const VERBOSE = process.argv.includes('--verbose') || process.argv.includes('-v');
const PS3MAPI_PORT = 80;

// Known cellUsbd NIDs from the current SPRX code
const NIDS_32 = [
  0x7F5F00D3,  // cellUsbdInit
  0x1AB6D80B,  // cellUsbdOpenPipe
  0x7B4436CE,  // cellUsbdTransfer
  0x2F82F1A5,  // cellUsbdClosePipe
];

const NID_NAMES = {
  '0x7F5F00D3': 'cellUsbdInit',
  '0x1AB6D80B': 'cellUsbdOpenPipe',
  '0x7B4436CE': 'cellUsbdTransfer',
  '0x2F82F1A5': 'cellUsbdClosePipe',
};

// These regions cover the PS3 memory map where game import stubs live
const SCAN_REGIONS = [
  { start: 0x00100000, size: 0x00800000, desc: 'game .text region 1' },
  { start: 0x01000000, size: 0x01000000, desc: 'game .text region 2' },
  { start: 0x02000000, size: 0x01000000, desc: 'game .text region 3' },
  { start: 0x30000000, size: 0x00800000, desc: 'cellUsbd PRX' },
  { start: 0x40000000, size: 0x01000000, desc: 'cellUsbd PRX alt' },
];

const CHUNK_SIZE = 0x8000; // 32KB chunks to avoid timeout
const POLL_MS = 200;

function getmem(pid, addr, len) {
  return new Promise((resolve, reject) => {
    const endpoint = `/getmem.ps3mapi?proc=${pid}&addr=0x${addr.toString(16)}&len=${len}`;
    const req = http.get(`http://${PS3_IP}:${PS3MAPI_PORT}${endpoint}`, (res) => {
      let data = '';
      res.on('data', chunk => data += chunk);
      res.on('end', () => resolve(data));
    });
    req.on('error', (err) => reject(err));
    req.setTimeout(10000, () => { req.destroy(); reject(new Error('timeout')); });
  });
}

/** Try to parse hex text from the getmem response HTML */
function extractHexFromResponse(resp) {
  // webMAN wraps in <textarea> or plain text
  const textareaMatch = resp.match(/<textarea[^>]*>([\s\S]*?)<\/textarea>/);
  const hexStr = textareaMatch ? textareaMatch[1].trim() : resp.trim();
  // It's a continuous hex string like "38A0000090BF0014..."
  if (/^[0-9a-fA-F]+$/.test(hexStr)) {
    return Buffer.from(hexStr, 'hex');
  }
  // Try to find hex digits even with whitespace
  const clean = hexStr.replace(/\s+/g, '');
  if (/^[0-9a-fA-F]+$/.test(clean)) {
    return Buffer.from(clean, 'hex');
  }
  return null;
}

async function scanMemory(pid) {
  console.log(`\n=== Scanning PS3 game memory for cellUsbd NIDs ===`);
  console.log(`PS3 IP: ${PS3_IP}`);
  console.log(`Game PID: 0x${pid.toString(16)} (${pid})\n`);
  console.log(`Trying NIDs (32-bit):`);
  for (const nid of NIDS_32) {
    const name = NID_NAMES[`0x${nid.toString(16).toUpperCase()}`] || 'unknown';
    console.log(`  0x${nid.toString(16).toUpperCase()} (${name})`);
  }
  console.log('');

  let totalFound = {};
  for (const nid of NIDS_32) {
    totalFound[nid] = [];
  }

  for (const region of SCAN_REGIONS) {
    const end = region.start + region.size;
    let addr = region.start;
    let chunkCount = 0;
    let foundInRegion = {};

    while (addr < end) {
      const chunkLen = Math.min(CHUNK_SIZE, end - addr);
      try {
        const resp = await getmem(pid, addr, chunkLen);
        const buf = extractHexFromResponse(resp);
        
        if (buf) {
          // Search for NIDs as little-endian 32-bit integers (as currently defined in code)
          for (const nid of NIDS_32) {
            if (foundInRegion[nid]) continue; // already found in this region
            // Try LE: nid bytes in memory order
            const nidLE = Buffer.alloc(4);
            nidLE.writeUInt32LE(nid, 0);
            let idx = buf.indexOf(nidLE);
            if (idx >= 0) {
              const physAddr = addr + idx;
              // Check what follows: the triplet format is NID + reserved(4) + GOT_ptr(4)
              const triplet = buf.slice(idx, idx + 12);
              const reserved = triplet.readUInt32LE(4);
              const gotPtr = triplet.readUInt32LE(8);
              const name = NID_NAMES[`0x${nid.toString(16).toUpperCase()}`] || 'UNKNOWN';
              console.log(`★ FOUND ${name} (NID=0x${nid.toString(16).toUpperCase()})`);
              console.log(`   Address: 0x${physAddr.toString(16)} (region: ${region.desc})`);
              console.log(`   Triplet: ${triplet.toString('hex').toUpperCase()}`);
              console.log(`   Reserved: 0x${reserved.toString(16)}`);
              console.log(`   GOT ptr: 0x${gotPtr.toString(16)}`);
              
              if (gotPtr > 0x00010000 && gotPtr < 0x4FFFFFFF) {
                // Try to read the GOT slot to get the actual function address
                try {
                  await sleep(POLL_MS);
                  const gotResp = await getmem(pid, gotPtr, 16);
                  const gotBuf = extractHexFromResponse(gotResp);
                  if (gotBuf && gotBuf.length >= 4) {
                    const funcAddr = gotBuf.readUInt32LE(0);
                    console.log(`   → GOT[0x${gotPtr.toString(16)}] = 0x${funcAddr.toString(16)}`);
                    if (funcAddr >= 0x30000000 && funcAddr <= 0x4FFFFFFF) {
                      console.log(`   ✓ This looks like the REAL cellUsbd function address!`);
                    } else if (funcAddr > 0x00100000 && funcAddr < 0x01FFFFFF) {
                      console.log(`   ⚠ This looks like a PLT STUB (lazy binding not resolved yet)`);
                    }
                  }
                } catch (e) {
                  // GOT read failed, expected if page is protected
                }
              }
              
              foundInRegion[nid] = true;
              totalFound[nid].push({ addr: physAddr, region: region.desc, reserved, gotPtr });
              console.log('');
            }

            // If not found as LE, try BE
            if (!foundInRegion[nid]) {
              const nidBE = Buffer.alloc(4);
              nidBE.writeUInt32BE(nid, 0);
              let idx = buf.indexOf(nidBE);
              if (idx >= 0) {
                const physAddr = addr + idx;
                const triplet = buf.slice(idx, idx + 12);
                const reserved = triplet.readUInt32BE(4);
                const gotPtr = triplet.readUInt32BE(8);
                const name = NID_NAMES[`0x${nid.toString(16).toUpperCase()}`] || 'UNKNOWN';
                console.log(`★ FOUND ${name} (NID=0x${nid.toString(16).toUpperCase()}) [BIG ENDIAN]`);
                console.log(`   Address: 0x${physAddr.toString(16)} (region: ${region.desc})`);
                console.log(`   Triplet: ${triplet.toString('hex').toUpperCase()}`);
                console.log(`   Reserved: 0x${reserved.toString(16)}`);
                console.log(`   GOT ptr: 0x${gotPtr.toString(16)}`);
                
                if (gotPtr > 0x00010000 && gotPtr < 0x4FFFFFFF) {
                  try {
                    await sleep(POLL_MS);
                    const gotResp = await getmem(pid, gotPtr, 16);
                    const gotBuf = extractHexFromResponse(gotResp);
                    if (gotBuf && gotBuf.length >= 4) {
                      const funcAddr = gotBuf.readUInt32BE(0);
                      console.log(`   → GOT[0x${gotPtr.toString(16)}] = 0x${funcAddr.toString(16)}`);
                      if (funcAddr >= 0x30000000 && funcAddr <= 0x4FFFFFFF) {
                        console.log(`   ✓ This looks like the REAL cellUsbd function address!`);
                      } else if (funcAddr > 0x00100000 && funcAddr < 0x01FFFFFF) {
                        console.log(`   ⚠ This looks like a PLT STUB (lazy binding not resolved yet)`);
                      }
                    }
                  } catch (e) {}
                }
                
                foundInRegion[nid] = true;
                totalFound[nid].push({ addr: physAddr, region: region.desc, reserved, gotPtr });
                console.log('');
              }
            }
          }
        }
      } catch (err) {
        // Memory read failure (probably protected page), skip silently
      }
      
      addr += CHUNK_SIZE;
      chunkCount++;
      
      // Progress for first region
      if (VERBOSE && chunkCount % 8 === 0 && region.start <= 0x00800000) {
        const pct = Math.round(((addr - region.start) / region.size) * 100);
        process.stdout.write(`  ${region.desc}: ${pct}% scanned (0x${addr.toString(16)})\r`);
      }
    }
    
    if (VERBOSE) {
      const found = Object.keys(foundInRegion).filter(k => foundInRegion[k]);
      if (found.length > 0) {
        console.log(`  ✓ ${region.desc}: found ${found.length} NIDs`);
      } else {
        console.log(`  - ${region.desc}: no NIDs found`);
      }
    }
  }

  // Summary
  console.log('\n═══ SCAN RESULTS ═══');
  for (const nid of NIDS_32) {
    const name = NID_NAMES[`0x${nid.toString(16).toUpperCase()}`] || 'UNKNOWN';
    if (totalFound[nid].length > 0) {
      console.log(`✓ ${name}: FOUND at ${totalFound[nid].map(f => `0x${f.addr.toString(16)}`).join(', ')}`);
    } else {
      console.log(`✗ ${name}: NOT FOUND in scanned regions`);
    }
  }

  if (Object.values(totalFound).flat().length === 0) {
    console.log('\n⚠ No NID stubs found with known 32-bit values.');
    console.log('  The game may use different NID values or a different stub format.');
    console.log('  Trying broader scan for any 3x 32-bit-word patterns that look like stubs...');
    console.log('  (This will dump first 256KB of game memory for manual analysis)');
    
    // Dump the first region raw for human inspection
    console.log('\n--- Raw dump of 0x00100000 (first 512 bytes) ---');
    try {
      const resp = await getmem(pid, 0x00100000, 512);
      const buf = extractHexFromResponse(resp);
      if (buf) {
        for (let i = 0; i < buf.length; i += 16) {
          const hex = buf.slice(i, i + 16).toString('hex').toUpperCase();
          const ascii = Array.from(buf.slice(i, i + 16))
            .map(b => b >= 32 && b < 127 ? String.fromCharCode(b) : '.')
            .join('');
          console.log(`  ${(0x00100000 + i).toString(16)}: ${hex}  ${ascii}`);
        }
      }
    } catch (e) {
      console.log(`  Read failed: ${e.message}`);
    }
  }
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function main() {
  // Resolve game PID
  let pid = null;
  const pidIdx = process.argv.indexOf('--pid');
  if (pidIdx !== -1 && process.argv[pidIdx + 1]) {
    const pidStr = process.argv[pidIdx + 1];
    pid = parseInt(pidStr.startsWith('0x') ? pidStr : '0x' + pidStr, 16);
  } else {
    // Default: LEGO Dimensions PID observed earlier
    pid = 0x1010200;
  }
  
  await scanMemory(pid);
  console.log('\nDone.');
}

main().catch(err => {
  console.error(`Fatal: ${err.message}`);
  process.exit(1);
});
